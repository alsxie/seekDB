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

#include "ob_hipvs_plugin_abi.h"

#include <cuvs/distance/distance.hpp>
#include <cuvs/neighbors/cagra.hpp>
#include <cuvs/neighbors/hnsw.hpp>
#include <raft/core/copy.hpp>
#include <raft/core/device_mdarray.hpp>
#include <raft/core/device_resources.hpp>
#include <raft/core/resource/cuda_stream.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void copy_error(char *destination, size_t capacity, const std::string &message)
{
  if (destination != nullptr && capacity > 0) {
    std::snprintf(destination, capacity, "%s", message.c_str());
  }
}

template <typename T>
bool read_value(const std::vector<char> &data, size_t &position, T &value)
{
  if (position > data.size() || data.size() - position < sizeof(T)) {
    return false;
  }
  std::memcpy(&value, data.data() + position, sizeof(T));
  position += sizeof(T);
  return true;
}

bool rewrite_labels(std::vector<char> &serialized,
                    const int64_t *ids,
                    int64_t expected_rows,
                    int32_t expected_degree,
                    std::string &error)
{
  size_t position = 0;
  size_t offset_level0 = 0;
  size_t max_elements = 0;
  size_t current_elements = 0;
  size_t element_size = 0;
  size_t label_offset = 0;
  size_t data_offset = 0;
  int max_level = 0;
  uint32_t entrypoint = 0;
  size_t max_m = 0;
  size_t max_m0 = 0;
  size_t m = 0;
  double multiplier = 0;
  size_t ef_construction = 0;
  if (!read_value(serialized, position, offset_level0) ||
      !read_value(serialized, position, max_elements) ||
      !read_value(serialized, position, current_elements) ||
      !read_value(serialized, position, element_size) ||
      !read_value(serialized, position, label_offset) ||
      !read_value(serialized, position, data_offset) ||
      !read_value(serialized, position, max_level) ||
      !read_value(serialized, position, entrypoint) ||
      !read_value(serialized, position, max_m) ||
      !read_value(serialized, position, max_m0) ||
      !read_value(serialized, position, m) ||
      !read_value(serialized, position, multiplier) ||
      !read_value(serialized, position, ef_construction)) {
    error = "truncated hipVS hnswlib header";
    return false;
  }
  if (current_elements != static_cast<size_t>(expected_rows) ||
      max_elements < current_elements || m != static_cast<size_t>(expected_degree) ||
      max_m != m || max_m0 != 2 * m || entrypoint >= current_elements || max_level < 0 ||
      label_offset > element_size || sizeof(int64_t) > element_size - label_offset) {
    error = "hipVS hnswlib metadata does not match seekDB request";
    return false;
  }
  if (current_elements > 0 &&
      (element_size > (std::numeric_limits<size_t>::max() - position) / current_elements ||
       position + element_size * current_elements > serialized.size())) {
    error = "invalid hipVS hnswlib level-zero payload";
    return false;
  }
  for (size_t row = 0; row < current_elements; ++row) {
    const size_t label_position = position + row * element_size + label_offset;
    std::memcpy(serialized.data() + label_position, ids + row, sizeof(int64_t));
  }
  return true;
}

std::string temporary_path()
{
  char path[] = "/tmp/seekdb-hipvs-XXXXXX";
  const int descriptor = mkstemp(path);
  if (descriptor < 0) {
    return {};
  }
  close(descriptor);
  return path;
}

void release_buffer(void *data)
{
  std::free(data);
}

} // namespace

extern "C" uint32_t seekdb_hipvs_get_abi_version(void)
{
  return SEEKDB_HIPVS_PLUGIN_ABI_VERSION;
}

extern "C" int seekdb_hipvs_build_hnsw(const SeekdbHipvsBuildRequest *request,
                                         SeekdbHipvsBuffer *output,
                                         char *error_message,
                                         size_t error_message_size)
{
  if (request == nullptr || output == nullptr || request->abi_version != SEEKDB_HIPVS_PLUGIN_ABI_VERSION ||
      request->vectors == nullptr || request->ids == nullptr || request->row_count <= 0 ||
      request->dim <= 0 || request->max_degree <= 0 || request->ef_construction <= 0) {
    copy_error(error_message, error_message_size, "invalid seekDB hipVS request");
    return 1;
  }

  output->data = nullptr;
  output->size = 0;
  output->release = nullptr;
  std::string path;
  try {
    raft::device_resources resources;
    auto dataset = raft::make_device_matrix<float, int64_t>(resources, request->row_count, request->dim);
    raft::copy(dataset.data_handle(),
               request->vectors,
               static_cast<size_t>(request->row_count) * request->dim,
               raft::resource::get_cuda_stream(resources));
    raft::resource::sync_stream(resources);

    const auto metric = request->metric == SEEKDB_HIPVS_METRIC_INNER_PRODUCT
                            ? cuvs::distance::DistanceType::InnerProduct
                            : cuvs::distance::DistanceType::L2Expanded;
    auto cagra_params = cuvs::neighbors::hnsw::to_cagra_params(
        dataset.extents(), request->max_degree, request->ef_construction, metric);
    auto cagra_index = cuvs::neighbors::cagra::build(
        resources, cagra_params, raft::make_const_mdspan(dataset.view()));

    cuvs::neighbors::hnsw::index_params hnsw_params;
    hnsw_params.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::GPU;
    hnsw_params.ef_construction = request->ef_construction;
    hnsw_params.num_threads = request->num_threads;
    auto hnsw_index = cuvs::neighbors::hnsw::from_cagra(resources, hnsw_params, cagra_index);

    path = temporary_path();
    if (path.empty()) {
      throw std::runtime_error("unable to create temporary hipVS index path");
    }
    cuvs::neighbors::hnsw::serialize(resources, path, *hnsw_index);

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
      throw std::runtime_error("unable to read serialized hipVS index");
    }
    const std::streamsize file_size = input.tellg();
    if (file_size <= 0) {
      throw std::runtime_error("hipVS produced an empty index");
    }
    input.seekg(0, std::ios::beg);
    std::vector<char> serialized(static_cast<size_t>(file_size));
    if (!input.read(serialized.data(), file_size)) {
      throw std::runtime_error("unable to load serialized hipVS index");
    }

    std::string rewrite_error;
    if (!rewrite_labels(serialized, request->ids, request->row_count, request->max_degree, rewrite_error)) {
      throw std::runtime_error(rewrite_error);
    }

    void *buffer = std::malloc(serialized.size());
    if (buffer == nullptr) {
      throw std::bad_alloc();
    }
    std::memcpy(buffer, serialized.data(), serialized.size());
    output->data = buffer;
    output->size = serialized.size();
    output->release = release_buffer;
    unlink(path.c_str());
    return 0;
  } catch (const std::exception &error) {
    if (!path.empty()) {
      unlink(path.c_str());
    }
    copy_error(error_message, error_message_size, error.what());
    return 2;
  }
}
