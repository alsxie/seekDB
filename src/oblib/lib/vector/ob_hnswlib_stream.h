/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author：FDE WorkMate
//
// Dependency-free helpers for hnswlib serialized streams that are produced by an
// external builder (for example hipVS) and imported into VSAG HNSW.
//
// The layout handled here is the one written by hnswlib::HierarchicalNSW::saveIndex
// and read by VSAG HierarchicalNSW::DeserializeImpl (new header format, 32-bit entry
// point):
//
//   size_t offset_level0, max_elements, cur_element_count, size_data_per_element,
//          label_offset, offset_data
//   int    max_level
//   uint32 entry_point
//   size_t max_m, max_m0, m
//   double mult
//   size_t ef_construction
//   level-0 block : cur_element_count * size_data_per_element bytes
//                   element = [uint32 count | max_m0 * uint32 neighbors][vector][int64 label]
//   upper levels  : per element { uint32 bytes; bytes of level * (uint32 count | max_m * uint32) }
//
// Normalized (cosine) indexes append a norm array that is not produced by external
// builders, so only non-normalized streams are accepted.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <streambuf>
#include <string>

namespace oceanbase {
namespace common {
namespace obvsag {
namespace hnswlib_stream {

struct Expectation {
  int64_t row_count;
  int32_t dim;
  int32_t max_degree;  // hnswlib M; level 0 holds 2 * M neighbors
};

struct Header {
  size_t offset_level0;
  size_t max_elements;
  size_t cur_element_count;
  size_t size_data_per_element;
  size_t label_offset;
  size_t offset_data;
  int max_level;
  uint32_t entry_point;
  size_t max_m;
  size_t max_m0;
  size_t m;
  double mult;
  size_t ef_construction;
};

constexpr size_t HEADER_SIZE = 6 * sizeof(size_t) + sizeof(int) + sizeof(uint32_t) +
                               3 * sizeof(size_t) + sizeof(double) + sizeof(size_t);

namespace detail {

template <typename T>
inline bool read_pod(const char *data, size_t size, size_t &position, T &value)
{
  if (position > size || size - position < sizeof(T)) {
    return false;
  }
  std::memcpy(&value, data + position, sizeof(T));
  position += sizeof(T);
  return true;
}

inline bool multiply(size_t a, size_t b, size_t &result)
{
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
    return false;
  }
  result = a * b;
  return true;
}

// hnswlib stores the neighbor count in the low 16 bits of the list header; the
// remaining bits carry flags such as "deleted".
inline uint32_t list_count(const char *list)
{
  uint16_t count = 0;
  std::memcpy(&count, list, sizeof(count));
  return count;
}

inline bool check_list(const char *list, size_t capacity, size_t element_count)
{
  const uint32_t count = list_count(list);
  if (count > capacity) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t neighbor = 0;
    std::memcpy(&neighbor, list + sizeof(uint32_t) * (1 + i), sizeof(neighbor));
    if (neighbor >= element_count) {
      return false;
    }
  }
  return true;
}

}  // namespace detail

inline bool parse_header(const char *data, size_t size, Header &header, std::string &error)
{
  size_t position = 0;
  if (data == nullptr ||
      !detail::read_pod(data, size, position, header.offset_level0) ||
      !detail::read_pod(data, size, position, header.max_elements) ||
      !detail::read_pod(data, size, position, header.cur_element_count) ||
      !detail::read_pod(data, size, position, header.size_data_per_element) ||
      !detail::read_pod(data, size, position, header.label_offset) ||
      !detail::read_pod(data, size, position, header.offset_data) ||
      !detail::read_pod(data, size, position, header.max_level) ||
      !detail::read_pod(data, size, position, header.entry_point) ||
      !detail::read_pod(data, size, position, header.max_m) ||
      !detail::read_pod(data, size, position, header.max_m0) ||
      !detail::read_pod(data, size, position, header.m) ||
      !detail::read_pod(data, size, position, header.mult) ||
      !detail::read_pod(data, size, position, header.ef_construction)) {
    error = "truncated hnswlib header";
    return false;
  }
  return true;
}

// Validates the whole stream against what seekDB requested and rewrites the
// external label of row i to ids[i]. The buffer is modified in place only when the
// stream is fully valid.
inline bool validate_and_relabel(char *data,
                                 size_t size,
                                 const Expectation &expected,
                                 const int64_t *ids,
                                 std::string &error)
{
  Header header{};
  if (!parse_header(data, size, header, error)) {
    return false;
  }
  if (ids == nullptr || expected.row_count <= 0 || expected.dim <= 0 || expected.max_degree <= 0) {
    error = "invalid hnswlib expectation";
    return false;
  }
  const size_t rows = static_cast<size_t>(expected.row_count);
  const size_t m = static_cast<size_t>(expected.max_degree);
  const size_t vector_bytes = static_cast<size_t>(expected.dim) * sizeof(float);
  const size_t level0_links = (2 * m + 1) * sizeof(uint32_t);
  const size_t upper_links = (m + 1) * sizeof(uint32_t);
  if (header.cur_element_count != rows || header.max_elements < rows) {
    error = "hnswlib element count does not match the request";
    return false;
  }
  if (header.m != m || header.max_m != m || header.max_m0 != 2 * m) {
    error = "hnswlib degree does not match the requested max_degree";
    return false;
  }
  if (header.offset_level0 != 0 || header.offset_data != level0_links ||
      header.label_offset != level0_links + vector_bytes ||
      header.size_data_per_element != level0_links + vector_bytes + sizeof(int64_t)) {
    error = "hnswlib element layout does not match the requested dimension";
    return false;
  }
  if (header.max_level < 0 || header.entry_point >= rows || !(header.mult > 0.0)) {
    error = "invalid hnswlib hierarchy metadata";
    return false;
  }

  size_t level0_bytes = 0;
  if (!detail::multiply(rows, header.size_data_per_element, level0_bytes) ||
      HEADER_SIZE > size || level0_bytes > size - HEADER_SIZE) {
    error = "truncated hnswlib level-zero block";
    return false;
  }
  const size_t level0_begin = HEADER_SIZE;
  for (size_t row = 0; row < rows; ++row) {
    const char *element = data + level0_begin + row * header.size_data_per_element;
    if (!detail::check_list(element, 2 * m, rows)) {
      error = "invalid hnswlib level-zero adjacency";
      return false;
    }
  }

  size_t position = level0_begin + level0_bytes;
  int top_level_seen = 0;
  int entry_point_level = 0;
  for (size_t row = 0; row < rows; ++row) {
    uint32_t link_bytes = 0;
    if (!detail::read_pod(data, size, position, link_bytes)) {
      error = "truncated hnswlib upper-level block";
      return false;
    }
    if (link_bytes % upper_links != 0 || link_bytes > size - position) {
      error = "invalid hnswlib upper-level list size";
      return false;
    }
    const size_t levels = link_bytes / upper_links;
    if (levels > static_cast<size_t>(header.max_level)) {
      error = "hnswlib element level exceeds max_level";
      return false;
    }
    for (size_t level = 0; level < levels; ++level) {
      if (!detail::check_list(data + position + level * upper_links, m, rows)) {
        error = "invalid hnswlib upper-level adjacency";
        return false;
      }
    }
    if (static_cast<int>(levels) > top_level_seen) {
      top_level_seen = static_cast<int>(levels);
    }
    if (row == header.entry_point) {
      entry_point_level = static_cast<int>(levels);
    }
    position += link_bytes;
  }
  if (position != size) {
    error = "unexpected trailing bytes in hnswlib stream";
    return false;
  }
  if (top_level_seen != header.max_level || entry_point_level != header.max_level) {
    error = "hnswlib entry point is not on the top level";
    return false;
  }

  for (size_t row = 0; row < rows; ++row) {
    char *label = data + level0_begin + row * header.size_data_per_element + header.label_offset;
    std::memcpy(label, ids + row, sizeof(int64_t));
  }
  return true;
}

// Read-only, seekable view over a memory buffer. VSAG's IOStreamReader seeks to the
// end to learn the stream length and Footer::Parse seeks to the tail, so a plain
// get-area streambuf is not enough.
class MemoryIStreamBuf final : public std::streambuf {
public:
  MemoryIStreamBuf(const char *data, size_t size)
  {
    char *begin = const_cast<char *>(data);
    setg(begin, begin, begin + size);
  }

protected:
  pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode which) override
  {
    if ((which & std::ios_base::in) == 0) {
      return pos_type(off_type(-1));
    }
    const off_type length = egptr() - eback();
    off_type base = 0;
    if (direction == std::ios_base::cur) {
      base = gptr() - eback();
    } else if (direction == std::ios_base::end) {
      base = length;
    }
    const off_type target = base + offset;
    if (target < 0 || target > length) {
      return pos_type(off_type(-1));
    }
    setg(eback(), eback() + target, egptr());
    return pos_type(target);
  }

  pos_type seekpos(pos_type position, std::ios_base::openmode which) override
  {
    return seekoff(off_type(position), std::ios_base::beg, which);
  }
};

}  // namespace hnswlib_stream
}  // namespace obvsag
}  // namespace common
}  // namespace oceanbase
