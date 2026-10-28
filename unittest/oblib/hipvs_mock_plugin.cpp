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
// GPU-free stand-in for libseekdb_hipvs.so. SEEKDB_HIPVS_MOCK_MODE selects the
// behaviour: ok (default), fail, oom, corrupt, empty, abi.

#include "hnswlib_stream_fixture.h"
#include "lib/vector/ob_hipvs_plugin_abi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#define MOCK_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

std::atomic<int> active_builds{0};
std::atomic<int> max_active_builds{0};
std::atomic<int> total_builds{0};

std::string mode()
{
  const char *value = std::getenv("SEEKDB_HIPVS_MOCK_MODE");
  return value == nullptr ? "ok" : value;
}

void release(void *data)
{
  std::free(data);
}

int finish(int result, char *error, size_t error_size, const char *message)
{
  std::snprintf(error, error_size, "%s", message);
  return result;
}

}  // namespace

MOCK_EXPORT uint32_t seekdb_hipvs_get_abi_version(void)
{
  return mode() == "abi" ? SEEKDB_HIPVS_PLUGIN_ABI_VERSION + 1 : SEEKDB_HIPVS_PLUGIN_ABI_VERSION;
}

MOCK_EXPORT int seekdb_hipvs_build_hnsw(const SeekdbHipvsBuildRequest *request,
                                        SeekdbHipvsBuffer *output,
                                        SeekdbHipvsBuildStats *stats,
                                        char *error,
                                        size_t error_size)
{
  const int active = ++active_builds;
  int observed = max_active_builds.load();
  while (active > observed && !max_active_builds.compare_exchange_weak(observed, active)) {
  }
  ++total_builds;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  int result = SEEKDB_HIPVS_OK;
  const std::string current = mode();
  stats->device_free_bytes = 1 << 20;
  stats->device_required_bytes = 1 << 10;
  if (current == "fail") {
    result = finish(SEEKDB_HIPVS_BUILD_ERROR, error, error_size, "mock build failure");
  } else if (current == "oom") {
    stats->device_required_bytes = stats->device_free_bytes * 2;
    result = finish(SEEKDB_HIPVS_INSUFFICIENT_DEVICE_MEMORY, error, error_size, "mock out of memory");
  } else if (current == "empty") {
    output->data = nullptr;
    output->size = 0;
    output->release = release;
  } else {
    const hnswlib_fixture::Layout layout{static_cast<size_t>(request->row_count), request->dim,
                                         request->max_degree};
    std::string stream = hnswlib_fixture::make_stream(layout);
    if (current == "corrupt") {
      stream.resize(stream.size() - 3);
    }
    void *buffer = std::malloc(stream.size());
    std::memcpy(buffer, stream.data(), stream.size());
    output->data = buffer;
    output->size = stream.size();
    output->release = release;
    stats->cagra_build_us = 11;
    stats->hierarchy_build_us = 22;
    stats->serialize_us = 33;
  }
  --active_builds;
  return result;
}

MOCK_EXPORT int seekdb_hipvs_mock_max_active_builds(void)
{
  return max_active_builds.load();
}

MOCK_EXPORT int seekdb_hipvs_mock_total_builds(void)
{
  return total_builds.load();
}
