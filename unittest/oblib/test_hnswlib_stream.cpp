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

#include "hnswlib_stream_fixture.h"
#include "lib/vector/ob_hnswlib_stream.h"

#include <cstring>
#include <iostream>
#include <istream>
#include <string>
#include <vector>

namespace stream = oceanbase::common::obvsag::hnswlib_stream;
using hnswlib_fixture::Layout;

namespace {

int failures = 0;

void expect(bool condition, const char *message)
{
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << std::endl;
  }
}

std::vector<int64_t> make_ids(size_t rows)
{
  std::vector<int64_t> ids(rows);
  for (size_t i = 0; i < rows; ++i) {
    ids[i] = 1000 + static_cast<int64_t>(i) * 7;
  }
  return ids;
}

bool accepts(std::string data, const Layout &layout, const stream::Expectation &expected)
{
  const std::vector<int64_t> ids = make_ids(layout.rows);
  std::string error;
  return stream::validate_and_relabel(&data[0], data.size(), expected, ids.data(), error);
}

template <typename T>
void overwrite(std::string &data, size_t position, T value)
{
  std::memcpy(&data[position], &value, sizeof(T));
}

void test_header_size()
{
  expect(stream::HEADER_SIZE == hnswlib_fixture::header_size(), "header size matches hnswlib saveIndex");
}

void test_relabel()
{
  const Layout layout{5, 3, 4};
  std::string data = hnswlib_fixture::make_stream(layout);
  const std::vector<int64_t> ids = make_ids(layout.rows);
  std::string error;
  const stream::Expectation expected{5, 3, 4};
  expect(stream::validate_and_relabel(&data[0], data.size(), expected, ids.data(), error),
         "valid stream is accepted");
  for (size_t row = 0; row < layout.rows; ++row) {
    expect(hnswlib_fixture::read_label(data, layout, row) == ids[row], "label rewritten to vector id");
  }
}

void test_rejections()
{
  const Layout layout{5, 3, 4};
  const std::string good = hnswlib_fixture::make_stream(layout);
  const stream::Expectation expected{5, 3, 4};

  expect(!accepts(good, layout, stream::Expectation{6, 3, 4}), "row count mismatch rejected");
  expect(!accepts(good, layout, stream::Expectation{5, 4, 4}), "dimension mismatch rejected");
  expect(!accepts(good, layout, stream::Expectation{5, 3, 8}), "degree mismatch rejected");
  expect(!accepts(good.substr(0, 40), layout, expected), "truncated header rejected");
  expect(!accepts(good.substr(0, good.size() - 1), layout, expected), "truncated tail rejected");
  expect(!accepts(good + std::string(16, '\0'), layout, expected), "trailing bytes rejected");

  // Level-0 neighbor id out of range.
  std::string bad_neighbor = good;
  overwrite<uint32_t>(bad_neighbor, stream::HEADER_SIZE + sizeof(uint32_t), 99U);
  expect(!accepts(bad_neighbor, layout, expected), "out-of-range neighbor rejected");

  // Neighbor count above level-0 capacity (2 * M).
  std::string bad_count = good;
  overwrite<uint32_t>(bad_count, stream::HEADER_SIZE, 9U);
  expect(!accepts(bad_count, layout, expected), "oversized adjacency rejected");

  // Entry point that is not on the top level.
  std::string bad_entry = good;
  overwrite<uint32_t>(bad_entry, 6 * sizeof(size_t) + sizeof(int), 2U);
  expect(!accepts(bad_entry, layout, expected), "entry point below top level rejected");

  // A rejected stream must not be modified.
  std::string untouched = bad_entry;
  accepts(untouched, layout, expected);
  expect(hnswlib_fixture::read_label(untouched, layout, 1) == 1, "rejected stream left unchanged");
}

// Mirrors what VSAG IOStreamReader and Footer::Parse do with the stream.
void test_seekable_buffer()
{
  const std::string payload = "0123456789abcdefghij";
  stream::MemoryIStreamBuf buffer(payload.data(), payload.size());
  std::istream input(&buffer);
  const std::streampos start = input.tellg();
  input.seekg(0, std::ios::end);
  const std::streamoff length = input.tellg() - start;
  input.seekg(start);
  expect(length == static_cast<std::streamoff>(payload.size()), "stream length visible via seekg/tellg");

  input.seekg(static_cast<std::streamoff>(payload.size() - 4));
  char tail[5] = {0};
  input.read(tail, 4);
  expect(input && std::string(tail) == "ghij", "tail readable after absolute seek");

  input.seekg(0);
  char head[5] = {0};
  input.read(head, 4);
  expect(input && std::string(head) == "0123", "head readable after rewind");

  input.seekg(static_cast<std::streamoff>(payload.size() + 1));
  expect(input.fail(), "seek past end fails");
}

}  // namespace

int main()
{
  test_header_size();
  test_relabel();
  test_rejections();
  test_seekable_buffer();
  if (failures == 0) {
    std::cout << "test_hnswlib_stream: all checks passed" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
