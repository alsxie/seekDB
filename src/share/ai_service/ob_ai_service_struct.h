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

#ifndef OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_
#define OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "common/json_type/ob_json_base.h"
#include "share/schema/ob_schema_struct.h"
#include "share/ai_service/ob_ai_model_info.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_unify_serialize.h"
#include "lib/lock/ob_spin_lock.h"
#include "common/mysqlclient/ob_isql_client.h"

namespace oceanbase
{
namespace share
{

// Layer-neutral AI service endpoint info from user side.
class ObAiModelEndpointInfo
{
  friend class ObAiServiceProxy;
public:
  ObAiModelEndpointInfo() { reset(); }
  ~ObAiModelEndpointInfo() = default;

  void reset()
  {
    name_.reset();
    scope_ = DEFAULT_SCOPE;
    ai_model_name_.reset();
    url_.reset();
    access_key_.reset();
    provider_.reset();
    request_model_name_.reset();
    parameters_.reset();
    request_transform_fn_.reset();
    response_transform_fn_.reset();
    max_concurrency_ = UNSPECIFIED_MAX_CONCURRENCY;
    endpoint_id_ = OB_INVALID_ID;
  }

  int parse_from_json_base(common::ObArenaAllocator &allocator,const ObString &name, const common::ObIJsonBase &params_jbase);
  int merge_delta_endpoint(common::ObArenaAllocator &allocator, const ObIJsonBase &delta_endpoint);
  int deep_copy(common::ObIAllocator &allocator, const ObAiModelEndpointInfo &other);
  int check_valid() const;

  const ObString &get_name() const { return name_; }
  const ObString &get_scope() const { return scope_; }
  const ObString &get_url() const { return url_; }
  const ObString &get_encrypted_access_key() const { return access_key_; }
  int get_unencrypted_access_key(common::ObIAllocator &allocator, ObString &unencrypted_access_key) const;
  const ObString &get_ai_model_name() const { return ai_model_name_; }
  const ObString &get_request_model_name() const { return request_model_name_; }
  const ObString &get_provider() const { return provider_; }
  const ObString &get_parameters() const { return parameters_; }
  const ObString &get_request_transform_fn() const { return request_transform_fn_; }
  const ObString &get_response_transform_fn() const { return response_transform_fn_; }
  // 0 means the endpoint does not limit request concurrency explicitly.
  int64_t get_max_concurrency() const { return max_concurrency_; }
  bool has_max_concurrency() const { return max_concurrency_ != UNSPECIFIED_MAX_CONCURRENCY; }
  uint64_t get_endpoint_id() const { return endpoint_id_; }
  void set_endpoint_id(uint64_t endpoint_id) { endpoint_id_ = endpoint_id; }

  TO_STRING_KV(K_(name),
               K_(scope),
               K_(endpoint_id),
               K_(url),
               K_(access_key),
               K_(ai_model_name),
               K_(provider),
               K_(request_model_name),
               K_(parameters),
               K_(request_transform_fn),
               K_(response_transform_fn),
               K_(max_concurrency));
  static constexpr int64_t UNSPECIFIED_MAX_CONCURRENCY = 0;
  static constexpr int64_t MIN_MAX_CONCURRENCY = 1;
  static constexpr int64_t MAX_MAX_CONCURRENCY = 1024;
  // Whether requests sent to this provider must carry an access key.
  static bool is_access_key_required(const ObString &provider);
private:
  static const ObString DEFAULT_SCOPE;
  static bool is_valid_provider(const ObString &provider);
  int parse_max_concurrency_(const ObIJsonBase &value);
  int encrypt_access_key_(common::ObIAllocator &allocator, const ObString &access_key, ObString &encrypted_access_key);
  int decrypt_access_key_(common::ObIAllocator &allocator, const ObString &encrypted_access_key, ObString &unencrypted_access_key) const;
private:
  uint64_t endpoint_id_;
  ObString name_;
  ObString scope_;
  ObString ai_model_name_;
  ObString url_;
  ObString access_key_;
  ObString provider_;
  ObString request_model_name_;
  ObString parameters_;
  ObString request_transform_fn_;
  ObString response_transform_fn_;
  int64_t max_concurrency_;
};

// ---------------------------------------------------------------------------
// AI column (asynchronous model generated column) shared definitions.
// 作者：FDE WorkMate
// ---------------------------------------------------------------------------

// Visible state of one AI column on one row, kept in the hidden state column.
enum AiColumnRowState
{
  AI_ROW_STATE_INVALID = 0,
  AI_ROW_STATE_PENDING = 1,
  AI_ROW_STATE_READY = 2,
  AI_ROW_STATE_FAILED = 3,
};

// Life cycle of one row of __all_ai_column_task.
enum AiColumnTaskState
{
  AI_TASK_STATE_INVALID = 0,
  AI_TASK_STATE_PENDING = 1,     // eligible once eligible_ts is reached
  AI_TASK_STATE_RUNNING = 2,     // leased by a worker until lease_expire_ts
  AI_TASK_STATE_RETRY_WAIT = 3,  // failed attempt, waits for eligible_ts
  AI_TASK_STATE_SUCCEEDED = 4,   // result kept in result_value, waits for its group
  AI_TASK_STATE_FAILED = 5,      // retries exhausted, waits for its group
};

class AiColumnUtil
{
public:
  static constexpr const char *SYNC_MODE_ASYNC = "async";
  static constexpr const char *FUNC_AI_COMPLETE = "AI_COMPLETE";
  static constexpr const char *FUNC_AI_EMBED = "AI_EMBED";
  static constexpr const char *SYSTEM_PROMPT_KEY = "system_prompt";
  static constexpr int64_t DEFAULT_MAX_RETRIES = 3;
  static constexpr int64_t MAX_MAX_RETRIES = 100;
  enum HiddenKind { HIDDEN_STATE = 0, HIDDEN_REQUEST = 1, HIDDEN_RESULT_SPEC = 2, HIDDEN_KIND_COUNT = 3 };
  // __ai_<column>_state / __ai_<column>_request / __ai_<column>_spec
  static int gen_hidden_column_name(common::ObIAllocator &allocator,
                                    const common::ObString &column_name,
                                    const HiddenKind kind,
                                    common::ObString &hidden_name);
  static const char *row_state_str(const int64_t state);
  // stable 63 bit identity of an AI column definition
  static int64_t calc_spec_version(const common::ObString &normalized_expr);
  // whether %name follows the hidden column naming scheme above
  static bool is_hidden_column_name(const common::ObString &name);
  // Reads the user facing definition of one AI column from __all_ai_column.
  static int load_column_definition(common::ObISQLClient &sql_client,
                                    common::ObIAllocator &allocator,
                                    const uint64_t table_id,
                                    const uint64_t column_id,
                                    common::ObString &original_expr,
                                    common::ObString &sync_mode,
                                    int64_t &max_retries);
  // Splits "FUNC(arg0, arg1, ...)" into the function name and the raw text of each top level
  // argument. Quotes, backticks and nested parentheses are honoured.
  static int split_call_args(const common::ObString &call_text,
                             common::ObString &func_name,
                             common::ObIArray<common::ObString> &args);
  // Fixed instruction that makes a completion model emit a value of %type, or an empty string
  // when the type needs no instruction (plain text).
  static const char *type_prompt(const common::ObObjType type);
  // Rewrites "AI_COMPLETE(model, prompt [, config])" so that the result type instruction is sent
  // as the system prompt. Other calls are copied unchanged.
  static int build_normalized_expr(common::ObIAllocator &allocator,
                                   const common::ObString &func_name,
                                   const common::ObIArray<common::ObString> &args,
                                   const common::ObObjType result_type,
                                   common::ObString &normalized_expr);
  // Replaces the bare identifier "status" (outside quotes and backticks) with %replacement.
  static int replace_status_pseudo_column(common::ObIAllocator &allocator,
                                          const common::ObString &text,
                                          const common::ObString &replacement,
                                          common::ObString &result);
};

// Persisted metadata of one AI result column, i.e. one row of __all_ai_column without the
// textual fields.
struct AiColumnMeta
{
  static const int64_t MAX_DEPENDENCY_COUNT = 64;
  AiColumnMeta() { reset(); }
  void reset();
  bool depends_on(const uint64_t column_id) const;
  TO_STRING_KV(K_(table_id), K_(column_id), K_(state_column_id), K_(request_column_id),
               K_(result_spec_column_id), K_(dependency_count), K_(max_retries),
               K_(spec_version));
  uint64_t table_id_;
  uint64_t column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  uint64_t dependency_column_ids_[MAX_DEPENDENCY_COUNT];
  int64_t dependency_count_;
  int64_t max_retries_;
  int64_t spec_version_;
};

// Process wide cache of __all_ai_column keyed by (table_id, schema_version).  Entries of an
// older schema version are simply replaced, so no explicit invalidation is needed.
class AiColumnMetaCache
{
public:
  static AiColumnMetaCache &get_instance();
  int get(common::ObISQLClient &sql_client,
          const uint64_t table_id,
          const int64_t schema_version,
          common::ObIArray<AiColumnMeta> &metas);
  static int load(common::ObISQLClient &sql_client,
                  const uint64_t table_id,
                  common::ObIArray<AiColumnMeta> &metas);
private:
  struct Entry
  {
    Entry() : table_id_(common::OB_INVALID_ID), schema_version_(0), metas_() {}
    TO_STRING_KV(K_(table_id), K_(schema_version));
    uint64_t table_id_;
    int64_t schema_version_;
    common::ObSEArray<AiColumnMeta, 2> metas_;
  };
  static const int64_t MAX_ENTRY_COUNT = 256;
  AiColumnMetaCache() : lock_(), entries_(), next_victim_(0) {}
  common::ObSpinLock lock_;
  common::ObSEArray<Entry, 8> entries_;
  int64_t next_victim_;
};

// AI column definition carried from the resolver to the DDL transaction.
// Column ids are bound by name inside the DDL transaction.
struct AiColumnDef
{
  OB_UNIS_VERSION(1);
public:
  AiColumnDef() { reset(); }
  void reset();
  bool is_valid() const { return !column_name_.empty() && !normalized_expr_.empty(); }
  // Builds "INSERT INTO __all_ai_column ..." for this definition.
  int gen_meta_insert_sql(const uint64_t table_id,
                          const uint64_t column_id,
                          const uint64_t state_column_id,
                          const uint64_t request_column_id,
                          const uint64_t result_spec_column_id,
                          const common::ObString &dependency_column_ids,
                          common::ObSqlString &sql) const;
  TO_STRING_KV(K_(column_name), K_(function_name), K_(model_name), K_(normalized_expr),
               K_(dependency_names), K_(sync_mode), K_(max_retries), K_(result_type),
               K_(spec_version));
public:
  common::ObString column_name_;
  common::ObString state_column_name_;
  common::ObString request_column_name_;
  common::ObString result_spec_column_name_;
  common::ObString function_name_;
  common::ObString model_name_;
  common::ObString original_expr_;
  common::ObString normalized_expr_;
  common::ObString dependency_names_;  // comma separated column names
  common::ObString sync_mode_;
  int64_t max_retries_;
  common::ObString result_type_;
  int64_t spec_version_;
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_
