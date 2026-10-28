/*
 * Copyright (c) 2025 OceanBase.
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

// 作者：FDE WorkMate
//
// Background worker of AI columns.  DML keeps one row of __all_ai_column_task per (row request,
// AI column) in the same transaction as the row change; this worker leases eligible tasks,
// evaluates the normalized AI expression against the current row, keeps the outcome in the task
// row and, once every task of a request group reached a terminal state, publishes the whole
// group into the user table in one transaction guarded by the request id.

#ifndef OCEANBASE_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_
#define OCEANBASE_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_

#include "share/ob_thread_pool.h"
#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/page_arena.h"

namespace oceanbase
{
namespace common
{
class ObISQLClient;
}
namespace observer
{

class ObAiColumnWorker : public share::ObThreadPool
{
public:
  static ObAiColumnWorker &get_instance();
  int start_worker();
  void stop_worker();
  virtual void run1() override;

  // knobs, shared with tests
  static const int64_t POLL_INTERVAL_US = 100 * 1000;
  static const int64_t LEASE_SECONDS = 60;
  static const int64_t BASE_BACKOFF_US = 1000 * 1000;
  static const int64_t MAX_BACKOFF_US = 60 * 1000 * 1000;
  static const int64_t CLAIM_BATCH = 32;
  static const int64_t GC_INTERVAL_US = 10 * 1000 * 1000;
  // request groups are spread over the threads by request id, so a group has a single owner
  static const int64_t THREAD_COUNT = 4;
  static int64_t calc_backoff_us(const int64_t retry_count);

private:
  struct Task
  {
    Task() : table_id_(0), request_id_(0), column_id_(0), state_(0), retry_count_(0) {}
    TO_STRING_KV(K_(table_id), K_(request_id), K_(column_id), K_(state), K_(retry_count),
                 K_(rowkey_predicate));
    uint64_t table_id_;
    int64_t request_id_;
    uint64_t column_id_;
    int64_t state_;
    int64_t retry_count_;
    common::ObString rowkey_predicate_;
  };
  struct ColumnInfo
  {
    ColumnInfo() : max_retries_(0) {}
    TO_STRING_KV(K_(database_name), K_(table_name), K_(column_name), K_(result_type),
                 K_(max_retries));
    common::ObString database_name_;
    common::ObString table_name_;
    common::ObString column_name_;
    common::ObString normalized_expr_;
    common::ObString result_type_;
    int64_t max_retries_;
  };

  ObAiColumnWorker() : last_gc_ts_(0), recovered_(false) {}
  int run_once(const int64_t thread_idx);
  int recover_leases();
  int fetch_tasks(common::ObIAllocator &allocator, const int64_t thread_idx,
                  common::ObIArray<Task> &tasks);
  int claim(const Task &task, bool &claimed);
  int process(common::ObIAllocator &allocator, const Task &task);
  int get_column_info(common::ObISQLClient &client, common::ObIAllocator &allocator,
                      const uint64_t table_id, const uint64_t column_id,
                      bool &exists, ColumnInfo &info);
  int compute(common::ObIAllocator &allocator, const Task &task, const ColumnInfo &info,
              bool &row_exists, bool &is_null, common::ObString &value);
  int validate(const ColumnInfo &info, const bool is_null, const common::ObString &value);
  int record_success(const Task &task, const bool is_null, const common::ObString &value);
  int record_failure(const Task &task, const int64_t max_retries, const int error);
  int delete_task(const Task &task);
  int try_publish(common::ObIAllocator &allocator, const uint64_t table_id,
                  const int64_t request_id);
  int gc();

  int64_t last_gc_ts_;    // owned by thread 0
  volatile bool recovered_;
};

}  // namespace observer
}  // namespace oceanbase

#endif  // OCEANBASE_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_
