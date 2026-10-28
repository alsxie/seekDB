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
// Writes small hnswlib streams in the exact layout of hnswlib::saveIndex so the
// import path can be tested without a GPU.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hnswlib_fixture {

struct Layout {
  size_t rows;
  int dim;
  int m;
};

template <typename T>
inline void put(std::string &out, const T &value)
{
  out.append(reinterpret_cast<const char *>(&value), sizeof(T));
}

inline void put_list(std::string &out, const std::vector<uint32_t> &neighbors, size_t capacity)
{
  put(out, static_cast<uint32_t>(neighbors.size()));
  for (size_t i = 0; i < capacity; ++i) {
    put(out, i < neighbors.size() ? neighbors[i] : 0U);
  }
}

// Row 0 is the entry point and the only element on level 1; every row links to its
// ring neighbors on level 0. Labels are row numbers, as an external builder emits.
inline std::string make_stream(const Layout &layout)
{
  const size_t m = static_cast<size_t>(layout.m);
  const size_t level0_links = (2 * m + 1) * sizeof(uint32_t);
  const size_t vector_bytes = static_cast<size_t>(layout.dim) * sizeof(float);
  const size_t element_size = level0_links + vector_bytes + sizeof(int64_t);
  std::string out;
  put(out, size_t(0));                         // offset_level0
  put(out, layout.rows);                       // max_elements
  put(out, layout.rows);                       // cur_element_count
  put(out, element_size);                      // size_data_per_element
  put(out, level0_links + vector_bytes);       // label_offset
  put(out, level0_links);                      // offset_data
  put(out, int(1));                            // max_level
  put(out, uint32_t(0));                       // entry point
  put(out, m);                                 // max_m
  put(out, 2 * m);                             // max_m0
  put(out, m);                                 // m
  put(out, 1.0 / 2.302585092994046);           // mult
  put(out, size_t(100));                       // ef_construction
  for (size_t row = 0; row < layout.rows; ++row) {
    std::vector<uint32_t> neighbors;
    if (layout.rows > 1) {
      neighbors.push_back(static_cast<uint32_t>((row + 1) % layout.rows));
      neighbors.push_back(static_cast<uint32_t>((row + layout.rows - 1) % layout.rows));
    }
    put_list(out, neighbors, 2 * m);
    for (int d = 0; d < layout.dim; ++d) {
      put(out, static_cast<float>(row * 10 + d));
    }
    put(out, static_cast<int64_t>(row));
  }
  for (size_t row = 0; row < layout.rows; ++row) {
    if (row == 0) {
      put(out, static_cast<uint32_t>((m + 1) * sizeof(uint32_t)));
      put_list(out, {}, m);
    } else {
      put(out, uint32_t(0));
    }
  }
  return out;
}

inline size_t header_size()
{
  return 6 * sizeof(size_t) + sizeof(int) + sizeof(uint32_t) + 3 * sizeof(size_t) + sizeof(double) +
         sizeof(size_t);
}

inline int64_t read_label(const std::string &stream, const Layout &layout, size_t row)
{
  const size_t m = static_cast<size_t>(layout.m);
  const size_t level0_links = (2 * m + 1) * sizeof(uint32_t);
  const size_t vector_bytes = static_cast<size_t>(layout.dim) * sizeof(float);
  const size_t element_size = level0_links + vector_bytes + sizeof(int64_t);
  int64_t label = 0;
  std::memcpy(&label, stream.data() + header_size() + row * element_size + level0_links + vector_bytes,
              sizeof(label));
  return label;
}

}  // namespace hnswlib_fixture
