/**
 * Copyright (C) 2019 leoetlino <leo@leolam.fr>
 *
 * This file is part of syaz0.
 *
 * syaz0 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * syaz0 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with syaz0.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cstring>

#include <oead/util/binary_reader.h>
#include <roead/src/yaz0.rs.h>

namespace oead::yaz0 {

constexpr std::array<char, 4> Magic = {'Y', 'a', 'z', '0'};
constexpr size_t ChunksPerGroup = 8;
constexpr size_t MaximumMatchLength = 0xFF + 0x12;

static Header GetHeader(util::BinaryReader& reader) {
  const auto header = reader.Read<Header>();
  if (header == std::nullopt || !header.has_value() )
    throw "No header";
  if (header->magic != Magic)
    throw "No header";
  return header.value();
}

Header GetHeader(rust::Slice<const u8> data) {
  util::BinaryReader reader{data, util::Endianness::Big};
  return GetHeader(reader);
}

namespace {
struct Match {
  size_t offset = 0;
  size_t length = 1;
};

// This deliberately follows the libyaz0 implementation used by
// Switch-Toolbox. Candidates are visited from oldest to newest, and ties keep
// the earliest candidate because only a strictly longer match replaces the
// current result.
Match FindMatch(rust::Slice<const u8> src, size_t pos, size_t search_range) {
  Match found;
  if (pos + 2 >= src.size() || search_range == 0)
    return found;

  const size_t search_begin = pos > search_range ? pos - search_range : 0;
  const size_t compare_end = std::min(src.size(), pos + MaximumMatchLength);
  for (size_t candidate = search_begin; candidate < pos; ++candidate) {
    if (src[candidate] != src[pos])
      continue;

    size_t source = candidate + 1;
    size_t current = pos + 1;
    while (current < compare_end && src[source] == src[current]) {
      ++source;
      ++current;
    }
    const size_t length = current - pos;
    if (length > found.length) {
      found = {candidate, length};
      if (length == MaximumMatchLength)
        break;
    }
  }
  return found;
}

size_t SearchRangeForLevel(int level) {
  level = std::clamp(level, 0, 9);
  if (level == 0)
    return 0;
  if (level < 9)
    return size_t(0x10e0 * level / 9 - 0x0e0);
  return 0x1000;
}
}  // namespace

rust::Vec<u8> Compress(rust::Slice<const u8> src, u32 data_alignment, int level) {
  util::BinaryWriter writer{util::Endianness::Big};
  writer.Buffer().reserve(src.size());

  // Write the header.
  Header header;
  header.magic = Magic;
  header.uncompressed_size = u32(src.size());
  header.data_alignment = data_alignment;
  header.reserved.fill(0);
  writer.Write(header);

  const size_t search_range = SearchRangeForLevel(level);
  size_t pos = 0;
  while (pos < src.size()) {
    const size_t code_offset = writer.Buffer().size();
    writer.Buffer().push_back(0);
    u8 code = 0;

    for (size_t chunk = 0; chunk < ChunksPerGroup && pos < src.size(); ++chunk) {
      const Match match = FindMatch(src, pos, search_range);
      if (match.length > 2) {
        const size_t delta = pos - match.offset - 1;
        if (match.length < 0x12) {
          writer.Buffer().push_back(
              u8((delta >> 8) | ((match.length - 2) << 4)));
          writer.Buffer().push_back(u8(delta));
        } else {
          writer.Buffer().push_back(u8(delta >> 8));
          writer.Buffer().push_back(u8(delta));
          writer.Buffer().push_back(u8(match.length - 0x12));
        }
        pos += match.length;
      } else {
        code |= u8(1 << (7 - chunk));
        writer.Buffer().push_back(src[pos++]);
      }
    }
    writer.Buffer()[code_offset] = code;
  }
  return writer.Finalize();
}

template <bool Safe>
static void Decompress(rust::Slice<const u8> src, rust::Slice<u8> dst) {
  util::BinaryReader reader{src, util::Endianness::Big};
  reader.Seek(sizeof(Header));

  u8 group_header = 0;
  size_t remaining_chunks = 0;
  for (auto dst_it = dst.begin(); dst_it < dst.end();) {
    if (remaining_chunks == 0) {
      group_header = reader.Read<u8, Safe>().value();
      remaining_chunks = ChunksPerGroup;
    }

    if (group_header & 0x80) {
      *dst_it++ = reader.Read<u8, Safe>().value();
    } else {
      const u16 pair = reader.Read<u16, Safe>().value();
      const size_t distance = (pair & 0x0FFF) + 1;
      const size_t length =
          ((pair >> 12) ? (pair >> 12) : (reader.Read<u8, Safe>().value() + 16)) + 2;

      const auto base = dst_it - distance;
      if (base < dst.begin() || dst_it + length > dst.end()) {
        throw std::invalid_argument("Copy is out of bounds");
      }
#pragma GCC unroll 1
      for (size_t i = 0; i < length; ++i)
        *dst_it++ = base[i];
    }

    group_header <<= 1;
    remaining_chunks -= 1;
  }
}

void Decompress(rust::Slice<const u8> src, rust::Slice<u8> dst) {
  Decompress<true>(src, dst);
}

void DecompressUnsafe(rust::Slice<const u8> src, rust::Slice<u8> dst) {
  Decompress<false>(src, dst);
}

}  // namespace oead::yaz0
