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

#include "ob_hipvs_builder.h"
#include "ob_hipvs_plugin_abi.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace oceanbase {
namespace common {
namespace obvsag {
namespace {

bool enabled_by_environment()
{
  const char *value = std::getenv("SEEKDB_HIPVS_ENABLE");
  return value != nullptr && (std::strcmp(value, "1") == 0 ||
                              std::strcmp(value, "true") == 0 ||
                              std::strcmp(value, "on") == 0);
}

int64_t minimum_rows()
{
  constexpr int64_t DEFAULT_MINIMUM_ROWS = 10000;
  const char *value = std::getenv("SEEKDB_HIPVS_MIN_ROWS");
  if (value == nullptr || *value == '\0') {
    return DEFAULT_MINIMUM_ROWS;
  }
  errno = 0;
  char *end = nullptr;
  const long long parsed = std::strtoll(value, &end, 10);
  return errno == 0 && end != value && *end == '\0' && parsed >= 0 ? parsed : DEFAULT_MINIMUM_ROWS;
}

bool resolve_metric(const char *metric, int32_t &resolved)
{
  bool supported = true;
  if (metric != nullptr && std::strcmp(metric, "l2") == 0) {
    resolved = SEEKDB_HIPVS_METRIC_L2;
  } else if (metric != nullptr && std::strcmp(metric, "ip") == 0) {
    resolved = SEEKDB_HIPVS_METRIC_INNER_PRODUCT;
  } else {
    supported = false;
  }
  return supported;
}

#if defined(__linux__)
template <typename Function>
Function load_function(void *library, const char *name)
{
  void *symbol = dlsym(library, name);
  Function function = nullptr;
  static_assert(sizeof(function) == sizeof(symbol), "function pointer size must match dlsym result");
  std::memcpy(&function, &symbol, sizeof(function));
  return function;
}
#endif

} // namespace

HipvsBuildStatus try_build_hipvs_hnsw(const float *vectors,
                                      const int64_t *ids,
                                      int64_t row_count,
                                      int dim,
                                      int max_degree,
                                      int ef_construction,
                                      const char *metric,
                                      std::string &serialized_index,
                                      std::string &error_message)
{
  serialized_index.clear();
  error_message.clear();
  if (!enabled_by_environment() || row_count < minimum_rows()) {
    return HipvsBuildStatus::NOT_REQUESTED;
  }
  if (vectors == nullptr || ids == nullptr || row_count <= 0 || dim <= 0 ||
      max_degree <= 0 || ef_construction <= 0 || row_count > INT_MAX) {
    error_message = "invalid hipVS build request";
    return HipvsBuildStatus::FAILED;
  }

  int32_t resolved_metric = 0;
  if (!resolve_metric(metric, resolved_metric)) {
    error_message = "hipVS supports seekDB l2 and inner-product HNSW indexes only";
    return HipvsBuildStatus::FAILED;
  }

#if !defined(__linux__)
  error_message = "hipVS acceleration is available on Linux ROCm hosts only";
  return HipvsBuildStatus::FAILED;
#else
  const char *configured_library = std::getenv("SEEKDB_HIPVS_LIBRARY");
  const char *library_path = configured_library != nullptr && *configured_library != '\0'
                                 ? configured_library
                                 : "libseekdb_hipvs.so";
  void *library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    const char *detail = dlerror();
    error_message = detail == nullptr ? "unable to load hipVS plugin" : detail;
    return HipvsBuildStatus::FAILED;
  }

  const auto get_abi_version = load_function<SeekdbHipvsGetAbiVersionFn>(library, "seekdb_hipvs_get_abi_version");
  const auto build_hnsw = load_function<SeekdbHipvsBuildHnswFn>(library, "seekdb_hipvs_build_hnsw");
  if (get_abi_version == nullptr || build_hnsw == nullptr ||
      get_abi_version() != SEEKDB_HIPVS_PLUGIN_ABI_VERSION) {
    error_message = "hipVS plugin ABI mismatch";
    dlclose(library);
    return HipvsBuildStatus::FAILED;
  }

  SeekdbHipvsBuildRequest request{};
  request.abi_version = SEEKDB_HIPVS_PLUGIN_ABI_VERSION;
  request.vectors = vectors;
  request.ids = ids;
  request.row_count = row_count;
  request.dim = dim;
  request.max_degree = max_degree;
  request.ef_construction = ef_construction;
  request.metric = resolved_metric;
  request.num_threads = 0;

  SeekdbHipvsBuffer output{};
  char error_buffer[512] = {0};
  const int plugin_result = build_hnsw(&request, &output, error_buffer, sizeof(error_buffer));
  if (plugin_result == 0 && output.data != nullptr && output.size > 0 && output.release != nullptr) {
    serialized_index.assign(static_cast<const char *>(output.data), output.size);
    output.release(output.data);
    dlclose(library);
    return HipvsBuildStatus::BUILT;
  }

  if (output.data != nullptr && output.release != nullptr) {
    output.release(output.data);
  }
  error_message = error_buffer[0] == '\0' ? "hipVS plugin build failed" : error_buffer;
  dlclose(library);
  return HipvsBuildStatus::FAILED;
#endif
}

} // namespace obvsag
} // namespace common
} // namespace oceanbase
