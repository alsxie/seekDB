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
// CPU reference implementation of the seekDB hipVS plugin ABI. It builds the graph
// with upstream hnswlib (the format hipVS serializes to) so the whole seekDB import
// path - bulk snapshot collection, stream validation, relabeling, VSAG import,
// persistence - can be exercised on hosts without an AMD GPU.
//
// Build (header-only hnswlib v0.8.0):
//   c++ -std=c++17 -O3 -fPIC -shared -I<hnswlib> -I src/oblib/lib/vector \
//       plugin/hipvs/tools/hnswlib_reference_plugin.cpp -o libseekdb_hnswlib_ref.so -lpthread

#include "ob_hipvs_plugin_abi.h"

#include <hnswlib/hnswlib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define REFERENCE_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

using Clock = std::chrono::steady_clock;

int64_t elapsed_us(Clock::time_point started_at)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started_at).count();
}

void release(void *data)
{
  std::free(data);
}

void *read_file(int descriptor, size_t &size)
{
  struct stat status {};
  if (fstat(descriptor, &status) != 0 || status.st_size <= 0) {
    return nullptr;
  }
  size = static_cast<size_t>(status.st_size);
  void *buffer = std::malloc(size);
  size_t done = 0;
  while (buffer != nullptr && done < size) {
    const ssize_t got = pread(descriptor, static_cast<char *>(buffer) + done, size - done, static_cast<off_t>(done));
    if (got <= 0) {
      std::free(buffer);
      return nullptr;
    }
    done += static_cast<size_t>(got);
  }
  return buffer;
}

}  // namespace

REFERENCE_EXPORT uint32_t seekdb_hipvs_get_abi_version(void)
{
  return SEEKDB_HIPVS_PLUGIN_ABI_VERSION;
}

REFERENCE_EXPORT int seekdb_hipvs_build_hnsw(const SeekdbHipvsBuildRequest *request,
                                             SeekdbHipvsBuffer *output,
                                             SeekdbHipvsBuildStats *stats,
                                             char *error,
                                             size_t error_size)
{
  if (request == nullptr || output == nullptr || stats == nullptr ||
      request->abi_version != SEEKDB_HIPVS_PLUGIN_ABI_VERSION || request->vectors == nullptr ||
      request->row_count <= 0 || request->dim <= 0 || request->max_degree <= 0) {
    std::snprintf(error, error_size, "invalid request");
    return SEEKDB_HIPVS_INVALID_REQUEST;
  }
  try {
    const size_t rows = static_cast<size_t>(request->row_count);
    const size_t dim = static_cast<size_t>(request->dim);
    std::unique_ptr<hnswlib::SpaceInterface<float>> space;
    if (request->metric == SEEKDB_HIPVS_METRIC_INNER_PRODUCT) {
      space.reset(new hnswlib::InnerProductSpace(dim));
    } else {
      space.reset(new hnswlib::L2Space(dim));
    }
    auto started_at = Clock::now();
    hnswlib::HierarchicalNSW<float> index(space.get(), rows, request->max_degree, request->ef_construction);
    const unsigned threads = request->num_threads > 0 ? request->num_threads
                                                       : std::max(1u, std::thread::hardware_concurrency());
    // Row 0 first so the entry point exists before parallel inserts.
    index.addPoint(request->vectors, 0);
    std::atomic<size_t> next{1};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
      workers.emplace_back([&]() {
        for (size_t row = next++; row < rows; row = next++) {
          index.addPoint(request->vectors + row * dim, row);
        }
      });
    }
    for (std::thread &worker : workers) {
      worker.join();
    }
    stats->cagra_build_us = elapsed_us(started_at);

    started_at = Clock::now();
    const int descriptor = memfd_create("seekdb-hnswlib-ref", MFD_CLOEXEC);
    if (descriptor < 0) {
      std::snprintf(error, error_size, "memfd_create failed");
      return SEEKDB_HIPVS_BUILD_ERROR;
    }
    index.saveIndex("/proc/self/fd/" + std::to_string(descriptor));
    size_t size = 0;
    void *buffer = read_file(descriptor, size);
    close(descriptor);
    stats->serialize_us = elapsed_us(started_at);
    if (buffer == nullptr) {
      std::snprintf(error, error_size, "unable to read serialized index");
      return SEEKDB_HIPVS_BUILD_ERROR;
    }
    output->data = buffer;
    output->size = size;
    output->release = release;
    return SEEKDB_HIPVS_OK;
  } catch (const std::exception &e) {
    std::snprintf(error, error_size, "%s", e.what());
    return SEEKDB_HIPVS_BUILD_ERROR;
  }
}
