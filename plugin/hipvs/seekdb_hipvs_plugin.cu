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
// hipVS build plugin: CAGRA graph on the GPU, converted to a full hnswlib HNSW
// (GPU-built hierarchy) and returned as a serialized stream with row-number labels.
// seekDB validates the stream, maps labels to vector ids and imports it into VSAG.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ob_hipvs_plugin_abi.h"

#include <cuvs/distance/distance.hpp>
#include <cuvs/neighbors/cagra.hpp>
#include <cuvs/neighbors/hnsw.hpp>
#include <hip/hip_runtime_api.h>
#include <raft/core/device_resources.hpp>
#include <raft/core/host_mdspan.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEEKDB_HIPVS_EXPORT __attribute__((visibility("default")))

namespace {

using Clock = std::chrono::steady_clock;

int64_t elapsed_us(Clock::time_point started_at)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started_at).count();
}

void copy_error(char *destination, size_t capacity, const std::string &message)
{
  if (destination != nullptr && capacity > 0) {
    std::snprintf(destination, capacity, "%s", message.c_str());
  }
}

// Conservative device-memory estimate for a CAGRA build from a host dataset with the
// IVF-PQ graph builder chosen by hnsw::to_cagra_params:
//   PQ codes        ~ rows * dim / 2 bytes (default pq_dim heuristic, 8-bit codes)
//   graph pruning   rows * 3M * 4 bytes (intermediate graph)
//   final graph     rows * 2M * 4 bytes
//   workspace       fixed 512 MiB for IVF-PQ search batches and hierarchy build
// The dataset itself stays on the host because attach_dataset_on_build is false.
int64_t estimate_device_bytes(int64_t rows, int32_t dim, int32_t m)
{
  const int64_t pq_codes = rows * dim / 2;
  const int64_t graphs = rows * (3LL * m + 2LL * m) * static_cast<int64_t>(sizeof(uint32_t));
  const int64_t workspace = 512LL << 20;
  return pq_codes + graphs + workspace;
}

// One stream and memory pool per build thread; created on first use and reused.
raft::device_resources &thread_resources()
{
  thread_local raft::device_resources resources;
  return resources;
}

// Serialization target that avoids touching the filesystem when memfd is available.
class SerializationFile {
public:
  SerializationFile()
  {
#ifdef MFD_CLOEXEC
    descriptor_ = memfd_create("seekdb-hipvs", MFD_CLOEXEC);
    if (descriptor_ >= 0) {
      path_ = "/proc/self/fd/" + std::to_string(descriptor_);
      return;
    }
#endif
    const char *directory = std::getenv("TMPDIR");
    std::string pattern = std::string(directory != nullptr && *directory != '\0' ? directory : "/tmp") +
                          "/seekdb-hipvs-XXXXXX";
    std::unique_ptr<char[]> buffer(new char[pattern.size() + 1]);
    std::memcpy(buffer.get(), pattern.c_str(), pattern.size() + 1);
    descriptor_ = mkstemp(buffer.get());
    if (descriptor_ < 0) {
      throw std::runtime_error("unable to create hipVS serialization file");
    }
    path_ = buffer.get();
    unlink_on_close_ = true;
  }

  ~SerializationFile()
  {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
    if (unlink_on_close_) {
      unlink(path_.c_str());
    }
  }

  const std::string &path() const { return path_; }

  // Reads the serialized index into a malloc buffer owned by the caller.
  void *read_all(size_t &size) const
  {
    struct stat status {};
    if (fstat(descriptor_, &status) != 0 || status.st_size <= 0) {
      throw std::runtime_error("hipVS produced an empty index");
    }
    size = static_cast<size_t>(status.st_size);
    void *buffer = std::malloc(size);
    if (buffer == nullptr) {
      throw std::bad_alloc();
    }
    size_t done = 0;
    while (done < size) {
      const ssize_t got = pread(descriptor_, static_cast<char *>(buffer) + done, size - done,
                                static_cast<off_t>(done));
      if (got <= 0) {
        std::free(buffer);
        throw std::runtime_error("unable to read serialized hipVS index");
      }
      done += static_cast<size_t>(got);
    }
    return buffer;
  }

private:
  int descriptor_ = -1;
  std::string path_;
  bool unlink_on_close_ = false;
};

void release_buffer(void *data)
{
  std::free(data);
}

} // namespace

extern "C" SEEKDB_HIPVS_EXPORT uint32_t seekdb_hipvs_get_abi_version(void)
{
  return SEEKDB_HIPVS_PLUGIN_ABI_VERSION;
}

extern "C" SEEKDB_HIPVS_EXPORT int seekdb_hipvs_build_hnsw(const SeekdbHipvsBuildRequest *request,
                                       SeekdbHipvsBuffer *output,
                                       SeekdbHipvsBuildStats *stats,
                                       char *error_message,
                                       size_t error_message_size)
{
  if (request == nullptr || output == nullptr || stats == nullptr ||
      request->abi_version != SEEKDB_HIPVS_PLUGIN_ABI_VERSION ||
      request->struct_size < sizeof(SeekdbHipvsBuildRequest) || request->vectors == nullptr ||
      request->row_count <= 0 || request->dim <= 0 || request->max_degree <= 0 ||
      request->ef_construction <= 0) {
    copy_error(error_message, error_message_size, "invalid seekDB hipVS request");
    return SEEKDB_HIPVS_INVALID_REQUEST;
  }
  output->data = nullptr;
  output->size = 0;
  output->release = nullptr;

  try {
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    if (hipMemGetInfo(&free_bytes, &total_bytes) != hipSuccess) {
      throw std::runtime_error("hipMemGetInfo failed");
    }
    stats->device_free_bytes = static_cast<int64_t>(free_bytes);
    stats->device_required_bytes =
        estimate_device_bytes(request->row_count, request->dim, request->max_degree);
    if (stats->device_required_bytes > stats->device_free_bytes) {
      copy_error(error_message, error_message_size, "insufficient GPU memory for hipVS HNSW build");
      return SEEKDB_HIPVS_INSUFFICIENT_DEVICE_MEMORY;
    }

    raft::device_resources &resources = thread_resources();
    auto dataset = raft::make_host_matrix_view<const float, int64_t>(
        request->vectors, request->row_count, request->dim);
    const auto metric = request->metric == SEEKDB_HIPVS_METRIC_INNER_PRODUCT
                            ? cuvs::distance::DistanceType::InnerProduct
                            : cuvs::distance::DistanceType::L2Expanded;

    auto started_at = Clock::now();
    auto cagra_params = cuvs::neighbors::hnsw::to_cagra_params(
        dataset.extents(), request->max_degree, request->ef_construction, metric);
    // The HNSW conversion reads vectors from the host copy below, so the device copy
    // of the dataset is not needed after graph construction.
    cagra_params.attach_dataset_on_build = false;
    auto cagra_index = cuvs::neighbors::cagra::build(resources, cagra_params, dataset);
    stats->cagra_build_us = elapsed_us(started_at);

    started_at = Clock::now();
    cuvs::neighbors::hnsw::index_params hnsw_params;
    hnsw_params.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::GPU;
    hnsw_params.ef_construction = request->ef_construction;
    hnsw_params.num_threads = request->num_threads;
    auto hnsw_index = cuvs::neighbors::hnsw::from_cagra(
        resources, hnsw_params, cagra_index, std::make_optional(dataset));
    stats->hierarchy_build_us = elapsed_us(started_at);

    started_at = Clock::now();
    SerializationFile file;
    cuvs::neighbors::hnsw::serialize(resources, file.path(), *hnsw_index);
    size_t size = 0;
    void *buffer = file.read_all(size);
    stats->serialize_us = elapsed_us(started_at);

    output->data = buffer;
    output->size = size;
    output->release = release_buffer;
    return SEEKDB_HIPVS_OK;
  } catch (const std::exception &error) {
    copy_error(error_message, error_message_size, error.what());
    return SEEKDB_HIPVS_BUILD_ERROR;
  }
}
