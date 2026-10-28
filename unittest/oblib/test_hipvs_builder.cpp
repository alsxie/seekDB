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
// Usage: test_hipvs_builder <path to hipvs mock plugin>

#include "hnswlib_stream_fixture.h"
#include "lib/vector/ob_hipvs_builder.h"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace oceanbase::common::obvsag;

namespace {

int failures = 0;

void expect(bool condition, const char *message)
{
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << std::endl;
  }
}

std::map<std::string, std::string> fake_environment;

const char *fake_lookup(const char *name)
{
  const auto found = fake_environment.find(name);
  return found == fake_environment.end() ? nullptr : found->second.c_str();
}

void set_mock_mode(const char *mode)
{
  setenv("SEEKDB_HIPVS_MOCK_MODE", mode, 1);
}

struct Dataset {
  static constexpr int ROWS = 64;
  static constexpr int DIM = 4;
  static constexpr int M = 8;
  std::vector<float> vectors = std::vector<float>(ROWS * DIM, 0.5f);
  std::vector<int64_t> ids = std::vector<int64_t>(ROWS);
  Dataset()
  {
    for (int i = 0; i < ROWS; ++i) {
      ids[i] = 90000 + i;
    }
  }
  HipvsBuildInput input(const char *metric = "l2") const
  {
    HipvsBuildInput in;
    in.vectors = vectors.data();
    in.ids = ids.data();
    in.row_count = ROWS;
    in.dim = DIM;
    in.max_degree = M;
    in.ef_construction = 100;
    in.metric = metric;
    return in;
  }
};

HipvsConfig enabled_config(const std::string &library)
{
  HipvsConfig config;
  config.enabled = true;
  config.min_rows = 0;
  config.library_path = library;
  return config;
}

void test_config_parsing()
{
  fake_environment.clear();
  HipvsConfig config = parse_hipvs_config(fake_lookup);
  expect(!config.enabled && config.min_rows == 10000 && config.max_concurrency == 1 &&
             config.library_path == "libseekdb_hipvs.so",
         "defaults without environment");

  fake_environment = {{"SEEKDB_HIPVS_ENABLE", "on"},
                      {"SEEKDB_HIPVS_MIN_ROWS", "500"},
                      {"SEEKDB_HIPVS_MAX_CONCURRENCY", "3"},
                      {"SEEKDB_HIPVS_LIBRARY", "/opt/x.so"}};
  config = parse_hipvs_config(fake_lookup);
  expect(config.enabled && config.min_rows == 500 && config.max_concurrency == 3 &&
             config.library_path == "/opt/x.so",
         "explicit environment values");

  fake_environment = {{"SEEKDB_HIPVS_ENABLE", "yes"},
                      {"SEEKDB_HIPVS_MIN_ROWS", "-1"},
                      {"SEEKDB_HIPVS_MAX_CONCURRENCY", "0"}};
  config = parse_hipvs_config(fake_lookup);
  expect(!config.enabled && config.min_rows == 10000 && config.max_concurrency == 1,
         "invalid values keep defaults");
}

void test_supported_shapes()
{
  expect(hipvs_supports("float32", "l2", 0), "float32 l2 supported");
  expect(hipvs_supports("float32", "ip", 0), "float32 ip supported");
  expect(!hipvs_supports("float32", "cosine", 0), "cosine not supported");
  expect(!hipvs_supports("float32", "l2", 8), "extra info not supported");
  expect(!hipvs_supports("sparse", "ip", 0), "sparse not supported");
}

void test_not_requested(const std::string &library)
{
  const Dataset data;
  HipvsPluginRuntime runtime(library, 1);
  HipvsHnswStream stream;
  HipvsBuildReport report;
  HipvsConfig disabled = enabled_config(library);
  disabled.enabled = false;
  expect(build_hnsw_with_hipvs(runtime, disabled, data.input(), stream, report) ==
             HipvsBuildStatus::NOT_REQUESTED,
         "disabled config is not requested");

  HipvsConfig large_minimum = enabled_config(library);
  large_minimum.min_rows = Dataset::ROWS + 1;
  expect(build_hnsw_with_hipvs(runtime, large_minimum, data.input(), stream, report) ==
             HipvsBuildStatus::NOT_REQUESTED,
         "below min_rows is not requested");
  expect(stream.data() == nullptr && report.error.empty(), "not requested leaves no output");
}

void test_early_failures(const std::string &library)
{
  const Dataset data;
  HipvsHnswStream stream;
  HipvsBuildReport report;
  HipvsPluginRuntime runtime(library, 1);
  expect(build_hnsw_with_hipvs(runtime, enabled_config(library), data.input("cosine"), stream, report) ==
             HipvsBuildStatus::FAILED && !report.error.empty(),
         "unsupported metric fails before plugin call");

  HipvsPluginRuntime missing("/nonexistent/libseekdb_hipvs.so", 1);
  expect(build_hnsw_with_hipvs(missing, enabled_config("/nonexistent/libseekdb_hipvs.so"), data.input(),
                               stream, report) == HipvsBuildStatus::FAILED && !report.error.empty(),
         "missing plugin fails with a diagnostic");
}

void test_plugin_success(const std::string &library)
{
  const Dataset data;
  set_mock_mode("ok");
  HipvsPluginRuntime runtime(library, 1);
  HipvsHnswStream stream;
  HipvsBuildReport report;
  expect(build_hnsw_with_hipvs(runtime, enabled_config(library), data.input(), stream, report) ==
             HipvsBuildStatus::BUILT,
         "mock plugin build succeeds");
  expect(stream.data() != nullptr && stream.size() > 0, "stream returned");
  expect(report.plugin_stats.cagra_build_us == 11 && report.plugin_stats.serialize_us == 33,
         "plugin stage timings propagated");
  const hnswlib_fixture::Layout layout{Dataset::ROWS, Dataset::DIM, Dataset::M};
  const std::string copy(stream.data(), stream.size());
  bool labels_ok = true;
  for (int row = 0; row < Dataset::ROWS; ++row) {
    labels_ok &= hnswlib_fixture::read_label(copy, layout, row) == data.ids[row];
  }
  expect(labels_ok, "row labels rewritten to vector ids");
}

void test_plugin_failures(const std::string &library)
{
  const Dataset data;
  const char *modes[] = {"fail", "oom", "corrupt", "empty"};
  for (const char *mode : modes) {
    set_mock_mode(mode);
    HipvsPluginRuntime runtime(library, 1);
    HipvsHnswStream stream;
    HipvsBuildReport report;
    const bool failed = build_hnsw_with_hipvs(runtime, enabled_config(library), data.input(), stream, report) ==
                        HipvsBuildStatus::FAILED;
    expect(failed && stream.data() == nullptr && !report.error.empty(), mode);
  }
  set_mock_mode("oom");
  {
    HipvsPluginRuntime runtime(library, 1);
    HipvsHnswStream stream;
    HipvsBuildReport report;
    build_hnsw_with_hipvs(runtime, enabled_config(library), data.input(), stream, report);
    expect(report.plugin_result == SEEKDB_HIPVS_INSUFFICIENT_DEVICE_MEMORY,
           "insufficient device memory reported");
  }
  set_mock_mode("abi");
  {
    HipvsPluginRuntime runtime(library, 1);
    std::string error;
    expect(!runtime.available(error) && error.find("ABI") != std::string::npos, "ABI mismatch rejected");
  }
  set_mock_mode("ok");
}

void test_concurrency_bound(const std::string &library)
{
  set_mock_mode("ok");
  void *handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
  expect(handle != nullptr, "mock plugin loadable for inspection");
  if (handle == nullptr) {
    return;
  }
  typedef int (*CounterFn)(void);
  const CounterFn max_active = reinterpret_cast<CounterFn>(dlsym(handle, "seekdb_hipvs_mock_max_active_builds"));
  const CounterFn total = reinterpret_cast<CounterFn>(dlsym(handle, "seekdb_hipvs_mock_total_builds"));
  const int before = total();

  const Dataset data;
  HipvsPluginRuntime runtime(library, 1);
  const HipvsConfig config = enabled_config(library);
  std::vector<std::thread> workers;
  std::vector<int> built(4, 0);
  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([&, i]() {
      HipvsHnswStream stream;
      HipvsBuildReport report;
      built[i] = build_hnsw_with_hipvs(runtime, config, data.input(), stream, report) == HipvsBuildStatus::BUILT;
    });
  }
  for (std::thread &worker : workers) {
    worker.join();
  }
  expect(built[0] && built[1] && built[2] && built[3], "concurrent builds succeed");
  expect(total() - before == 4, "every build reached the plugin");
  expect(max_active() == 1, "max_concurrency=1 serializes GPU builds");
  dlclose(handle);
}

}  // namespace

int main(int argc, char **argv)
{
  const char *from_env = std::getenv("SEEKDB_HIPVS_MOCK_PLUGIN");
  const std::string library = argc > 1 ? argv[1] : (from_env == nullptr ? "" : from_env);
  if (library.empty()) {
    std::cerr << "usage: test_hipvs_builder <mock plugin path>" << std::endl;
    return 2;
  }
  test_config_parsing();
  test_supported_shapes();
  test_not_requested(library);
  test_early_failures(library);
  test_plugin_success(library);
  test_plugin_failures(library);
  test_concurrency_bound(library);
  if (failures == 0) {
    std::cout << "test_hipvs_builder: all checks passed" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
