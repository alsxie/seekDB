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

#include "ob_hipvs_builder.h"
#include "ob_hnswlib_stream.h"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>

#if (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
#define SEEKDB_HIPVS_HAS_DLOPEN 1
#include <dlfcn.h>
#else
#define SEEKDB_HIPVS_HAS_DLOPEN 0
#endif

namespace oceanbase {
namespace common {
namespace obvsag {
namespace {

int64_t hipvs_elapsed_us(std::chrono::steady_clock::time_point started_at)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started_at).count();
}

bool parse_flag(const char *value)
{
  return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
                              std::strcmp(value, "on") == 0);
}

bool parse_integer(const char *value, long long minimum, long long maximum, long long &parsed)
{
  if (value == nullptr || *value == '\0') {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const long long result = std::strtoll(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || result < minimum || result > maximum) {
    return false;
  }
  parsed = result;
  return true;
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

const char *process_environment(const char *name)
{
  return std::getenv(name);
}

#if SEEKDB_HIPVS_HAS_DLOPEN
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

HipvsConfig parse_hipvs_config(HipvsEnvironmentLookup lookup)
{
  HipvsConfig config;
  if (lookup == nullptr) {
    return config;
  }
  config.enabled = parse_flag(lookup("SEEKDB_HIPVS_ENABLE"));
  long long parsed = 0;
  if (parse_integer(lookup("SEEKDB_HIPVS_MIN_ROWS"), 0, LLONG_MAX, parsed)) {
    config.min_rows = parsed;
  }
  if (parse_integer(lookup("SEEKDB_HIPVS_MAX_CONCURRENCY"), 1, 64, parsed)) {
    config.max_concurrency = static_cast<int32_t>(parsed);
  }
  const char *library = lookup("SEEKDB_HIPVS_LIBRARY");
  if (library != nullptr && *library != '\0') {
    config.library_path = library;
  }
  return config;
}

const HipvsConfig &hipvs_config()
{
  static const HipvsConfig config = parse_hipvs_config(process_environment);
  return config;
}

bool hipvs_supports(const char *dtype, const char *metric, int64_t extra_info_size)
{
  int32_t resolved = 0;
  return dtype != nullptr && std::strcmp(dtype, "float32") == 0 && extra_info_size == 0 &&
         resolve_metric(metric, resolved);
}

void HipvsHnswStream::reset()
{
  if (buffer_.data != nullptr && buffer_.release != nullptr) {
    buffer_.release(buffer_.data);
  }
  buffer_ = SeekdbHipvsBuffer{nullptr, 0, nullptr};
}

void HipvsHnswStream::adopt(const SeekdbHipvsBuffer &buffer)
{
  reset();
  buffer_ = buffer;
}

HipvsPluginRuntime::HipvsPluginRuntime(const std::string &library_path, int32_t max_concurrency)
    : library_path_(library_path), max_concurrency_(max_concurrency > 0 ? max_concurrency : 1)
{
}

HipvsPluginRuntime::~HipvsPluginRuntime()
{
#if SEEKDB_HIPVS_HAS_DLOPEN
  if (library_ != nullptr) {
    dlclose(library_);
  }
#endif
}

void HipvsPluginRuntime::load()
{
#if SEEKDB_HIPVS_HAS_DLOPEN
  void *library = dlopen(library_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    const char *detail = dlerror();
    load_error_ = detail == nullptr ? "unable to load hipVS plugin" : detail;
    return;
  }
  const auto get_abi_version =
      load_function<SeekdbHipvsGetAbiVersionFn>(library, "seekdb_hipvs_get_abi_version");
  const auto build = load_function<SeekdbHipvsBuildHnswFn>(library, "seekdb_hipvs_build_hnsw");
  if (get_abi_version == nullptr || build == nullptr ||
      get_abi_version() != SEEKDB_HIPVS_PLUGIN_ABI_VERSION) {
    load_error_ = "hipVS plugin ABI mismatch";
    dlclose(library);
    return;
  }
  library_ = library;
  build_fn_ = build;
#else
  load_error_ = "hipVS plugins are not supported on this platform";
#endif
}

bool HipvsPluginRuntime::available(std::string &error)
{
  std::call_once(load_once_, [this]() { load(); });
  if (build_fn_ == nullptr) {
    error = load_error_;
    return false;
  }
  return true;
}

int HipvsPluginRuntime::build(const SeekdbHipvsBuildRequest &request,
                              SeekdbHipvsBuffer &output,
                              SeekdbHipvsBuildStats &stats,
                              int64_t &queue_wait_us,
                              std::string &error)
{
  if (!available(error)) {
    return SEEKDB_HIPVS_BUILD_ERROR;
  }
  const auto queued_at = std::chrono::steady_clock::now();
  {
    std::unique_lock<std::mutex> lock(slot_mutex_);
    slot_released_.wait(lock, [this]() { return active_builds_ < max_concurrency_; });
    ++active_builds_;
  }
  queue_wait_us = hipvs_elapsed_us(queued_at);

  char error_buffer[512] = {0};
  const int result = build_fn_(&request, &output, &stats, error_buffer, sizeof(error_buffer));
  {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    --active_builds_;
  }
  slot_released_.notify_one();
  if (result != SEEKDB_HIPVS_OK) {
    error = error_buffer[0] == '\0' ? "hipVS plugin build failed" : error_buffer;
  }
  return result;
}

HipvsPluginRuntime &default_hipvs_runtime()
{
  static HipvsPluginRuntime *runtime =
      new HipvsPluginRuntime(hipvs_config().library_path, hipvs_config().max_concurrency);
  return *runtime;
}

bool hipvs_bulk_build_available(const char *dtype, const char *metric, int64_t extra_info_size)
{
  std::string error;
  return hipvs_config().enabled && hipvs_supports(dtype, metric, extra_info_size) &&
         default_hipvs_runtime().available(error);
}

HipvsBuildStatus build_hnsw_with_hipvs(HipvsPluginRuntime &runtime,
                                       const HipvsConfig &config,
                                       const HipvsBuildInput &input,
                                       HipvsHnswStream &stream,
                                       HipvsBuildReport &report)
{
  stream.reset();
  report = HipvsBuildReport();
  if (!config.enabled || input.row_count < config.min_rows) {
    return HipvsBuildStatus::NOT_REQUESTED;
  }
  if (input.vectors == nullptr || input.ids == nullptr || input.row_count <= 0 || input.dim <= 0 ||
      input.max_degree <= 0 || input.ef_construction <= 0 || input.row_count > INT_MAX) {
    report.error = "invalid hipVS build request";
    return HipvsBuildStatus::FAILED;
  }
  int32_t metric = 0;
  if (!resolve_metric(input.metric, metric)) {
    report.error = "hipVS supports l2 and inner-product HNSW indexes only";
    return HipvsBuildStatus::FAILED;
  }

  SeekdbHipvsBuildRequest request{};
  request.abi_version = SEEKDB_HIPVS_PLUGIN_ABI_VERSION;
  request.struct_size = sizeof(request);
  request.vectors = input.vectors;
  request.row_count = input.row_count;
  request.dim = input.dim;
  request.max_degree = input.max_degree;
  request.ef_construction = input.ef_construction;
  request.metric = metric;
  request.num_threads = 0;
  report.plugin_stats.struct_size = sizeof(report.plugin_stats);

  SeekdbHipvsBuffer output{nullptr, 0, nullptr};
  const auto plugin_started_at = std::chrono::steady_clock::now();
  report.plugin_result = runtime.build(request, output, report.plugin_stats, report.queue_wait_us, report.error);
  report.plugin_us = hipvs_elapsed_us(plugin_started_at) - report.queue_wait_us;
  stream.adopt(output);
  if (report.plugin_result != SEEKDB_HIPVS_OK) {
    stream.reset();
    return HipvsBuildStatus::FAILED;
  }
  if (stream.data() == nullptr || stream.size() == 0 || output.release == nullptr) {
    stream.reset();
    report.error = "hipVS plugin returned an empty stream";
    return HipvsBuildStatus::FAILED;
  }

  const auto validate_started_at = std::chrono::steady_clock::now();
  const hnswlib_stream::Expectation expected{input.row_count, input.dim, input.max_degree};
  const bool valid = hnswlib_stream::validate_and_relabel(stream.data(), stream.size(), expected,
                                                          input.ids, report.error);
  report.validate_us = hipvs_elapsed_us(validate_started_at);
  if (!valid) {
    stream.reset();
    return HipvsBuildStatus::FAILED;
  }
  return HipvsBuildStatus::BUILT;
}

} // namespace obvsag
} // namespace common
} // namespace oceanbase
