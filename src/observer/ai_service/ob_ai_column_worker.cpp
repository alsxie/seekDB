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

#define USING_LOG_PREFIX SERVER

#include "observer/ai_service/ob_ai_column_worker.h"
#include "share/ob_server_struct.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/utility/utility.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace share;
namespace observer
{

static const char *TASK_TABLE = "__all_ai_column_task";
static const char *META_TABLE = "__all_ai_column";

// `name` with embedded backticks doubled
static int append_ident(ObSqlString &sql, const ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = '`' == name.ptr()[i] ? sql.append("``") : sql.append(name.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`");
  }
  return ret;
}

// a string literal in utf8mb4 that survives any byte content
static int append_text_literal(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("CONVERT("))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(value, sql))) {
  } else if (OB_FAIL(sql.append(" USING utf8mb4)"))) {
  }
  return ret;
}

// Reads a string column of an inner SQL result, unwrapping inline lob locators.
static int get_string_cell(ObMySQLResult &result, const int64_t idx, ObIAllocator &allocator,
                           bool &is_null, ObString &value)
{
  int ret = OB_SUCCESS;
  ObObj obj;
  ObString raw;
  is_null = false;
  value.reset();
  if (OB_FAIL(result.get_obj(idx, obj))) {
    LOG_WARN("fail to get obj", K(ret), K(idx));
  } else if (obj.is_null()) {
    is_null = true;
  } else if (obj.is_lob_storage()) {
    raw = obj.get_string();
    if (obj.has_lob_header()) {
      ObLobLocatorV2 locator(raw, true);
      if (!locator.has_inrow_data()) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("out-of-row lob result is not supported", K(ret));
      } else if (OB_FAIL(locator.get_inrow_data(raw))) {
        LOG_WARN("fail to get inrow lob data", K(ret));
      }
    }
  } else if (obj.is_string_type()) {
    raw = obj.get_string();
  } else {
    char buf[128];
    int64_t pos = 0;
    if (OB_FAIL(obj.print_plain_str_literal(buf, sizeof(buf), pos))) {
    } else {
      raw.assign_ptr(buf, static_cast<int32_t>(pos));
      ret = ob_write_string(allocator, raw, value);
      return ret;
    }
  }
  if (OB_SUCC(ret) && !is_null && OB_FAIL(ob_write_string(allocator, raw, value))) {
  }
  return ret;
}

ObAiColumnWorker &ObAiColumnWorker::get_instance()
{
  static ObAiColumnWorker instance;
  return instance;
}

int64_t ObAiColumnWorker::calc_backoff_us(const int64_t retry_count)
{
  int64_t backoff = BASE_BACKOFF_US;
  for (int64_t i = 0; i < retry_count && backoff < MAX_BACKOFF_US; ++i) {
    backoff *= 2;
  }
  return backoff < MAX_BACKOFF_US ? backoff : MAX_BACKOFF_US;
}

int ObAiColumnWorker::start_worker()
{
  int ret = OB_SUCCESS;
  set_thread_count(THREAD_COUNT);
  if (OB_FAIL(start())) {
    LOG_WARN("fail to start ai column worker", K(ret));
  } else {
    LOG_INFO("ai column worker started");
  }
  return ret;
}

void ObAiColumnWorker::stop_worker()
{
  stop();
  wait();
}

void ObAiColumnWorker::run1()
{
  lib::set_thread_name("AiColumnWorker");
  const int64_t thread_idx = static_cast<int64_t>(get_thread_idx());
  while (!has_set_stop()) {
    if (SS_SERVING != GCTX.status_ || OB_ISNULL(GCTX.sql_proxy_)) {
    } else if (!recovered_) {
      // tasks leased before a restart belong to nobody: thread 0 releases them first
      if (0 == thread_idx) {
        int tmp_ret = recover_leases();
        if (OB_SUCCESS != tmp_ret) {
          LOG_WARN_RET(tmp_ret, "fail to recover ai column task leases");
        } else {
          recovered_ = true;
        }
      }
    } else {
      int tmp_ret = run_once(thread_idx);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN_RET(tmp_ret, "ai column worker round failed");
      }
      if (0 == thread_idx && ObTimeUtility::current_time() - last_gc_ts_ > GC_INTERVAL_US) {
        last_gc_ts_ = ObTimeUtility::current_time();
        if (OB_SUCCESS != (tmp_ret = gc())) {
          LOG_WARN_RET(tmp_ret, "ai column metadata gc failed");
        }
      }
    }
    ob_usleep(POLL_INTERVAL_US);
  }
}

int ObAiColumnWorker::recover_leases()
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append_fmt("UPDATE %s.%s SET state = %d WHERE state = %d",
                             OB_SYS_DATABASE_NAME, TASK_TABLE, AI_TASK_STATE_PENDING,
                             AI_TASK_STATE_RUNNING))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to release ai column task leases", K(ret));
  } else {
    LOG_INFO("released ai column task leases", K(affected_rows));
  }
  return ret;
}

int ObAiColumnWorker::run_once(const int64_t thread_idx)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColWorker");
  ObSEArray<Task, CLAIM_BATCH> tasks;
  if (OB_FAIL(fetch_tasks(allocator, thread_idx, tasks))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < tasks.count(); ++i) {
    bool claimed = false;
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = claim(tasks.at(i), claimed))) {
      LOG_WARN("fail to claim ai column task", K(tmp_ret), K(tasks.at(i)));
    } else if (claimed && OB_SUCCESS != (tmp_ret = process(allocator, tasks.at(i)))) {
      LOG_WARN("fail to process ai column task", K(tmp_ret), K(tasks.at(i)));
    }
  }
  return ret;
}

int ObAiColumnWorker::fetch_tasks(ObIAllocator &allocator, const int64_t thread_idx,
                                  ObIArray<Task> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("SELECT table_id, request_id, column_id, state, retry_count, "
                             "rowkey_predicate FROM %s.%s "
                             "WHERE ((state IN (%d, %d) AND eligible_ts <= now(6)) "
                             "OR (state = %d AND lease_expire_ts < now(6)) "
                             "OR state IN (%d, %d)) AND MOD(request_id, %ld) = %ld "
                             "ORDER BY eligible_ts LIMIT %ld",
                             OB_SYS_DATABASE_NAME, TASK_TABLE,
                             AI_TASK_STATE_PENDING, AI_TASK_STATE_RETRY_WAIT,
                             AI_TASK_STATE_RUNNING,
                             AI_TASK_STATE_SUCCEEDED, AI_TASK_STATE_FAILED,
                             get_thread_count(), thread_idx, CLAIM_BATCH))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("fail to read ai column tasks", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        Task task;
        bool is_null = false;
        EXTRACT_INT_FIELD_MYSQL(*result, "table_id", task.table_id_, uint64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "request_id", task.request_id_, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "column_id", task.column_id_, uint64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "state", task.state_, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "retry_count", task.retry_count_, int64_t);
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(get_string_cell(*result, 5, allocator, is_null,
                                           task.rowkey_predicate_))) {
        } else if (OB_FAIL(tasks.push_back(task))) {
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

// Terminal tasks are not leased again: their group only waits for a publish attempt.
int ObAiColumnWorker::claim(const Task &task, bool &claimed)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  claimed = false;
  if (AI_TASK_STATE_SUCCEEDED == task.state_ || AI_TASK_STATE_FAILED == task.state_) {
    claimed = true;
  } else if (OB_FAIL(sql.append_fmt("UPDATE %s.%s SET state = %d, "
                                    "lease_expire_ts = date_add(now(6), interval %ld second) "
                                    "WHERE table_id = %lu AND request_id = %ld AND column_id = %lu "
                                    "AND state = %ld",
                                    OB_SYS_DATABASE_NAME, TASK_TABLE, AI_TASK_STATE_RUNNING,
                                    LEASE_SECONDS, task.table_id_, task.request_id_,
                                    task.column_id_, task.state_))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to lease ai column task", K(ret), K(sql));
  } else {
    claimed = 1 == affected_rows;
  }
  return ret;
}

int ObAiColumnWorker::process(ObIAllocator &allocator, const Task &task)
{
  int ret = OB_SUCCESS;
  if (AI_TASK_STATE_SUCCEEDED != task.state_ && AI_TASK_STATE_FAILED != task.state_) {
    ColumnInfo info;
    bool exists = false;
    bool row_exists = false;
    bool is_null = false;
    ObString value;
    if (OB_FAIL(get_column_info(*GCTX.sql_proxy_, allocator, task.table_id_, task.column_id_,
                                exists, info))) {
    } else if (!exists) {
      // the table or the AI column is gone
      ret = delete_task(task);
    } else {
      int compute_ret = compute(allocator, task, info, row_exists, is_null, value);
      if (OB_SUCCESS == compute_ret && !row_exists) {
        // the row was deleted or got a newer request, this one can never be published
        ret = delete_task(task);
      } else if (OB_SUCCESS == compute_ret
                 && OB_SUCCESS == (compute_ret = validate(info, is_null, value))) {
        ret = record_success(task, is_null, value);
      } else {
        LOG_INFO("ai column request failed", K(compute_ret), K(task), K(info));
        ret = record_failure(task, info.max_retries_, compute_ret);
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(try_publish(allocator, task.table_id_, task.request_id_))) {
    LOG_WARN("fail to publish ai column group", K(ret), K(task));
  }
  return ret;
}

int ObAiColumnWorker::get_column_info(ObISQLClient &client, ObIAllocator &allocator,
                                      const uint64_t table_id, const uint64_t column_id,
                                      bool &exists, ColumnInfo &info)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  exists = false;
  if (OB_FAIL(sql.append_fmt("SELECT d.database_name, t.table_name, a.column_name, "
                             "a.normalized_expr, a.result_type, a.max_retries "
                             "FROM %s.%s a JOIN %s.__all_table t ON t.table_id = a.table_id "
                             "JOIN %s.__all_database d ON d.database_id = t.database_id "
                             "JOIN %s.__all_column c ON c.table_id = a.table_id "
                             "AND c.column_id = a.column_id "
                             "WHERE a.table_id = %lu AND a.column_id = %lu",
                             OB_SYS_DATABASE_NAME, META_TABLE, OB_SYS_DATABASE_NAME,
                             OB_SYS_DATABASE_NAME, OB_SYS_DATABASE_NAME, table_id, column_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      bool is_null = false;
      if (OB_FAIL(client.read(res, sql.ptr()))) {
        LOG_WARN("fail to read ai column info", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      } else if (OB_FAIL(get_string_cell(*result, 0, allocator, is_null, info.database_name_))
                 || OB_FAIL(get_string_cell(*result, 1, allocator, is_null, info.table_name_))
                 || OB_FAIL(get_string_cell(*result, 2, allocator, is_null, info.column_name_))
                 || OB_FAIL(get_string_cell(*result, 3, allocator, is_null, info.normalized_expr_))
                 || OB_FAIL(get_string_cell(*result, 4, allocator, is_null, info.result_type_))) {
      } else {
        EXTRACT_INT_FIELD_MYSQL(*result, "max_retries", info.max_retries_, int64_t);
        exists = OB_SUCC(ret);
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::compute(ObIAllocator &allocator, const Task &task, const ColumnInfo &info,
                              bool &row_exists, bool &is_null, ObString &value)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObString request_name;
  row_exists = false;
  is_null = false;
  if (OB_FAIL(AiColumnUtil::gen_hidden_column_name(allocator, info.column_name_,
                                                   AiColumnUtil::HIDDEN_REQUEST, request_name))) {
  } else if (OB_FAIL(sql.append_fmt("SELECT (%.*s) AS v FROM ", info.normalized_expr_.length(),
                                    info.normalized_expr_.ptr()))) {
  } else if (OB_FAIL(append_ident(sql, info.database_name_)) || OB_FAIL(sql.append("."))
             || OB_FAIL(append_ident(sql, info.table_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" WHERE (%.*s) AND ", task.rowkey_predicate_.length(),
                                    task.rowkey_predicate_.ptr()))) {
  } else if (OB_FAIL(append_ident(sql, request_name))
             || OB_FAIL(sql.append_fmt(" = %ld", task.request_id_))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("fail to evaluate ai column", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      } else if (OB_FAIL(get_string_cell(*result, 0, allocator, is_null, value))) {
      } else {
        row_exists = true;
      }
    }
  }
  return ret;
}

// Checks that the model output fits the declared column type before it can join its group.
int ObAiColumnWorker::validate(const ColumnInfo &info, const bool is_null, const ObString &value)
{
  int ret = OB_SUCCESS;
  const ObString &type = info.result_type_;
  ObString trimmed = value;
  trimmed = trimmed.trim();
  if (is_null) {
  } else if (0 == type.case_compare("json")) {
    ObSqlString sql;
    if (OB_FAIL(sql.append("SELECT JSON_VALID("))) {
    } else if (OB_FAIL(append_text_literal(sql, value))) {
    } else if (OB_FAIL(sql.append(") AS v"))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        ObMySQLResult *result = NULL;
        int64_t valid = 0;
        if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(result->next())) {
        } else if (OB_FAIL(result->get_int(0L, valid))) {
        } else if (1 != valid) {
          ret = OB_ERR_INVALID_JSON_TEXT;
          LOG_INFO("ai column result is not valid json", K(info), K(value));
        }
      }
    }
  } else if (type.prefix_match_ci("bigint") || type.prefix_match_ci("int")
             || type.prefix_match_ci("smallint") || type.prefix_match_ci("tinyint")
             || type.prefix_match_ci("mediumint")) {
    int64_t i = 0;
    if (i < trimmed.length() && ('-' == trimmed.ptr()[i] || '+' == trimmed.ptr()[i])) {
      ++i;
    }
    bool all_digits = i < trimmed.length();
    for (; all_digits && i < trimmed.length(); ++i) {
      all_digits = trimmed.ptr()[i] >= '0' && trimmed.ptr()[i] <= '9';
    }
    if (!all_digits) {
      ret = OB_INVALID_NUMERIC;
      LOG_INFO("ai column result is not an integer", K(info), K(value));
    }
  }
  // other types are converted (and so checked) by the publishing UPDATE
  return ret;
}

int ObAiColumnWorker::record_success(const Task &task, const bool is_null, const ObString &value)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append_fmt("UPDATE %s.%s SET state = %d, last_error = NULL, result_value = ",
                             OB_SYS_DATABASE_NAME, TASK_TABLE, AI_TASK_STATE_SUCCEEDED))) {
  } else if (is_null ? OB_FAIL(sql.append("NULL")) : OB_FAIL(append_text_literal(sql, value))) {
  } else if (OB_FAIL(sql.append_fmt(" WHERE table_id = %lu AND request_id = %ld AND column_id = %lu"
                                    " AND state = %d",
                                    task.table_id_, task.request_id_, task.column_id_,
                                    AI_TASK_STATE_RUNNING))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to record ai column result", K(ret), K(task));
  }
  return ret;
}

int ObAiColumnWorker::record_failure(const Task &task, const int64_t max_retries, const int error)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSqlString message;
  int64_t affected_rows = 0;
  const bool exhausted = task.retry_count_ >= max_retries;
  if (OB_FAIL(message.append_fmt("%d: %s", error, ob_strerror(error)))) {
  } else if (exhausted) {
    ret = sql.append_fmt("UPDATE %s.%s SET state = %d, result_value = NULL, last_error = ",
                         OB_SYS_DATABASE_NAME, TASK_TABLE, AI_TASK_STATE_FAILED);
  } else {
    ret = sql.append_fmt("UPDATE %s.%s SET state = %d, retry_count = retry_count + 1, "
                         "eligible_ts = date_add(now(6), interval %ld microsecond), last_error = ",
                         OB_SYS_DATABASE_NAME, TASK_TABLE, AI_TASK_STATE_RETRY_WAIT,
                         calc_backoff_us(task.retry_count_));
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(append_text_literal(sql, message.string()))) {
  } else if (OB_FAIL(sql.append_fmt(" WHERE table_id = %lu AND request_id = %ld AND column_id = %lu"
                                    " AND state IN (%d, %d)",
                                    task.table_id_, task.request_id_, task.column_id_,
                                    AI_TASK_STATE_RUNNING, AI_TASK_STATE_SUCCEEDED))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to record ai column failure", K(ret), K(task));
  }
  return ret;
}

int ObAiColumnWorker::delete_task(const Task &task)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append_fmt("DELETE FROM %s.%s WHERE table_id = %lu AND request_id = %ld "
                             "AND column_id = %lu",
                             OB_SYS_DATABASE_NAME, TASK_TABLE, task.table_id_, task.request_id_,
                             task.column_id_))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to delete ai column task", K(ret), K(task));
  }
  return ret;
}

// Publishes a request group once all of its tasks are terminal: the results of every AI column
// of the group become visible together, guarded by the request id so that a newer request of
// the row is never overwritten.  A result the column type rejects sends its task back to retry.
int ObAiColumnWorker::try_publish(ObIAllocator &allocator, const uint64_t table_id,
                                  const int64_t request_id)
{
  int ret = OB_SUCCESS;
  struct GroupTask
  {
    Task task_;
    bool is_null_;
    ObString value_;
    ColumnInfo info_;
    bool exists_;
  };
  ObSEArray<GroupTask, 4> group;
  bool complete = true;
  ObMySQLTransaction trans;
  ObSqlString sql;
  int64_t affected_rows = 0;
  int64_t failed_idx = -1;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    LOG_WARN("fail to start trans", K(ret));
  } else if (OB_FAIL(sql.append_fmt("SELECT column_id, state, retry_count, rowkey_predicate, "
                                    "result_value FROM %s.%s WHERE table_id = %lu "
                                    "AND request_id = %ld ORDER BY column_id FOR UPDATE",
                                    OB_SYS_DATABASE_NAME, TASK_TABLE, table_id, request_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(trans.read(res, sql.ptr()))) {
        LOG_WARN("fail to read ai column group", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && complete && OB_SUCC(result->next())) {
        GroupTask item;
        bool is_null = false;
        item.task_.table_id_ = table_id;
        item.task_.request_id_ = request_id;
        item.exists_ = false;
        EXTRACT_INT_FIELD_MYSQL(*result, "column_id", item.task_.column_id_, uint64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "state", item.task_.state_, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "retry_count", item.task_.retry_count_, int64_t);
        if (OB_FAIL(ret)) {
        } else if (AI_TASK_STATE_SUCCEEDED != item.task_.state_
                   && AI_TASK_STATE_FAILED != item.task_.state_) {
          complete = false;
        } else if (OB_FAIL(get_string_cell(*result, 3, allocator, is_null,
                                           item.task_.rowkey_predicate_))) {
        } else if (OB_FAIL(get_string_cell(*result, 4, allocator, item.is_null_, item.value_))) {
        } else if (OB_FAIL(group.push_back(item))) {
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  if (OB_SUCC(ret) && complete && group.count() > 0) {
    for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
      GroupTask &item = group.at(i);
      ObString state_name;
      ObString request_name;
      const bool succeeded = AI_TASK_STATE_SUCCEEDED == item.task_.state_;
      sql.reuse();
      if (OB_FAIL(get_column_info(trans, allocator, table_id, item.task_.column_id_,
                                  item.exists_, item.info_))) {
      } else if (!item.exists_) {
      } else if (OB_FAIL(AiColumnUtil::gen_hidden_column_name(
                     allocator, item.info_.column_name_, AiColumnUtil::HIDDEN_STATE, state_name))) {
      } else if (OB_FAIL(AiColumnUtil::gen_hidden_column_name(
                     allocator, item.info_.column_name_, AiColumnUtil::HIDDEN_REQUEST,
                     request_name))) {
      } else if (OB_FAIL(sql.append("UPDATE ")) || OB_FAIL(append_ident(sql, item.info_.database_name_))
                 || OB_FAIL(sql.append(".")) || OB_FAIL(append_ident(sql, item.info_.table_name_))
                 || OB_FAIL(sql.append(" SET ")) || OB_FAIL(append_ident(sql, item.info_.column_name_))
                 || OB_FAIL(sql.append(" = "))) {
      } else if ((succeeded && !item.is_null_) ? OB_FAIL(append_text_literal(sql, item.value_))
                                               : OB_FAIL(sql.append("NULL"))) {
      } else if (OB_FAIL(sql.append(", ")) || OB_FAIL(append_ident(sql, state_name))
                 || OB_FAIL(sql.append_fmt(" = %d WHERE (%.*s) AND ",
                                           succeeded ? AI_ROW_STATE_READY : AI_ROW_STATE_FAILED,
                                           item.task_.rowkey_predicate_.length(),
                                           item.task_.rowkey_predicate_.ptr()))
                 || OB_FAIL(append_ident(sql, request_name))
                 || OB_FAIL(sql.append_fmt(" = %ld", request_id))) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
        LOG_INFO("fail to publish ai column result", K(ret), K(item.task_), K(item.info_));
        failed_idx = succeeded ? i : -1;
      }
    }
    sql.reuse();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s.%s WHERE table_id = %lu AND request_id = %ld",
                                      OB_SYS_DATABASE_NAME, TASK_TABLE, table_id, request_id))) {
    } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
      LOG_WARN("fail to remove published ai column tasks", K(ret));
    }
  }
  if (trans.is_started()) {
    int tmp_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to end trans", K(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }
  if (OB_FAIL(ret) && failed_idx >= 0) {
    // the column type rejected the result: this counts as a failed attempt of that task
    const GroupTask &item = group.at(failed_idx);
    int tmp_ret = record_failure(item.task_, item.info_.max_retries_, ret);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to record publish failure", K(tmp_ret));
    } else {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

// Drops metadata and tasks of tables that no longer exist (recycle-bin tables still exist).
int ObAiColumnWorker::gc()
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  const char *tables[] = { META_TABLE, TASK_TABLE };
  for (int64_t i = 0; OB_SUCC(ret) && i < 2; ++i) {
    sql.reuse();
    if (OB_FAIL(sql.append_fmt("DELETE FROM %s.%s WHERE NOT EXISTS (SELECT 1 FROM %s.__all_column c "
                               "WHERE c.table_id = %s.table_id AND c.column_id = %s.column_id)",
                               OB_SYS_DATABASE_NAME, tables[i], OB_SYS_DATABASE_NAME,
                               tables[i], tables[i]))) {
    } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
      LOG_WARN("fail to gc ai column rows", K(ret), K(sql));
    } else if (affected_rows > 0) {
      LOG_INFO("removed ai column rows of dropped tables", K(tables[i]), K(affected_rows));
    }
  }
  return ret;
}

}  // namespace observer
}  // namespace oceanbase
