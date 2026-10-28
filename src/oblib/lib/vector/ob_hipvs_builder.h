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

#pragma once

#include "ob_hipvs_plugin_abi.h"

#include <condition_variable>
#include <mutex>
#include <stdint.h>
#include <string>

namespace oceanbase {
namespace common {
namespace obvsag {

enum class HipvsBuildStatus {
  NOT_REQUESTED,
  BUILT,
  FAILED,
};

struct HipvsConfig {
  bool enabled = false;
  int64_t min_rows = 10000;
  int32_t max_concurrency = 1;
  std::string library_path = "libseekdb_hipvs.so";
};

typedef const char *(*HipvsEnvironmentLookup)(const char *name);

// SEEKDB_HIPVS_ENABLE / SEEKDB_HIPVS_MIN_ROWS / SEEKDB_HIPVS_MAX_CONCURRENCY /
// SEEKDB_HIPVS_LIBRARY. Invalid values keep the defaults.
HipvsConfig parse_hipvs_config(HipvsEnvironmentLookup lookup);
// Process-wide configuration, read from the environment once.
const HipvsConfig &hipvs_config();

// Data shapes the GPU builder can produce a VSAG-importable stream for.
bool hipvs_supports(const char *dtype, const char *metric, int64_t extra_info_size);

// Owns the stream returned by the plugin; released through the plugin allocator.
class HipvsHnswStream {
public:
  HipvsHnswStream() = default;
  ~HipvsHnswStream() { reset(); }
  HipvsHnswStream(const HipvsHnswStream &) = delete;
  HipvsHnswStream &operator=(const HipvsHnswStream &) = delete;

  void reset();
  void adopt(const SeekdbHipvsBuffer &buffer);
  char *data() const { return static_cast<char *>(buffer_.data); }
  size_t size() const { return buffer_.size; }

private:
  SeekdbHipvsBuffer buffer_{nullptr, 0, nullptr};
};

struct HipvsBuildInput {
  const float *vectors = nullptr;
  const int64_t *ids = nullptr;
  int64_t row_count = 0;
  int dim = 0;
  int max_degree = 0;
  int ef_construction = 0;
  const char *metric = nullptr;
};

struct HipvsBuildReport {
  SeekdbHipvsBuildStats plugin_stats{};
  int64_t queue_wait_us = 0;
  int64_t plugin_us = 0;
  int64_t validate_us = 0;
  int plugin_result = SEEKDB_HIPVS_OK;
  std::string error;
};

// Loads the plugin once, keeps it resident and bounds concurrent GPU builds.
class HipvsPluginRuntime {
public:
  HipvsPluginRuntime(const std::string &library_path, int32_t max_concurrency);
  ~HipvsPluginRuntime();
  HipvsPluginRuntime(const HipvsPluginRuntime &) = delete;
  HipvsPluginRuntime &operator=(const HipvsPluginRuntime &) = delete;

  bool available(std::string &error);
  // Blocks until a build slot is free, then calls the plugin.
  int build(const SeekdbHipvsBuildRequest &request,
            SeekdbHipvsBuffer &output,
            SeekdbHipvsBuildStats &stats,
            int64_t &queue_wait_us,
            std::string &error);

private:
  void load();

  const std::string library_path_;
  const int32_t max_concurrency_;
  std::once_flag load_once_;
  void *library_ = nullptr;
  SeekdbHipvsBuildHnswFn build_fn_ = nullptr;
  std::string load_error_;
  std::mutex slot_mutex_;
  std::condition_variable slot_released_;
  int32_t active_builds_ = 0;
};

// Runtime bound to hipvs_config(); never destroyed so the plugin stays loaded.
HipvsPluginRuntime &default_hipvs_runtime();

// True when the process is configured for GPU builds, the data shape is supported
// and the plugin can be loaded. Callers use it to decide whether to collect the full
// dataset before building.
bool hipvs_bulk_build_available(const char *dtype, const char *metric, int64_t extra_info_size);

// Builds on the GPU and returns a validated stream whose labels are input.ids.
HipvsBuildStatus build_hnsw_with_hipvs(HipvsPluginRuntime &runtime,
                                       const HipvsConfig &config,
                                       const HipvsBuildInput &input,
                                       HipvsHnswStream &stream,
                                       HipvsBuildReport &report);

} // namespace obvsag
} // namespace common
} // namespace oceanbase
