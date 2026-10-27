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

#include "share/ai_service/ob_ai_service_struct.h"
#include "lib/json/ob_json.h"
#include "lib/hash_func/murmur_hash.h"
#include "lib/utility/utility.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/object/ob_obj_type.h"


#define USING_LOG_PREFIX SHARE

using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::json;

namespace oceanbase
{
namespace share
{
const ObString ObAiModelEndpointInfo::DEFAULT_SCOPE = "ALL";

struct ObAiProviderTrait
{
  const char *name_;
  // Local runtimes such as Ollama serve requests without authentication.
  bool access_key_required_;
};

const ObAiProviderTrait VALID_PROVIDERS[] = {
  {"ALIYUN-OPENAI", true},
  {"ALIYUN-DASHSCOPE", true},
  {"DEEPSEEK", true},
  {"SILICONFLOW", true},
  {"COHERE", true},
  {"HUNYUAN-OPENAI", true},
  {"OPENAI", true},
  {"OLLAMA", false}
};

#define EXTRACT_JSON_ELEM_STR(json_key, member) \
  EXTRACT_JSON_ELEM_STR_WITH_PROCESS(json_key, member, "void")

#define EXTRACT_JSON_ELEM_STR_WITH_PROCESS(json_key, member, post_process) \
      if (elem.first.case_compare(json_key) == 0) { \
        if (elem.second->json_type() != ObJsonNodeType::J_STRING) { \
          ret = OB_AI_FUNC_PARAM_VALUE_INVALID; \
          LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, elem.first.length(), elem.first.ptr()); \
          LOG_WARN("invalid json type", K(ret), K(elem.first), K(elem.second->json_type())); \
        } else { \
          member = ObString(elem.second->get_data_length(), elem.second->get_data()); \
          post_process; \
        } \
      } else

#define EXTRACT_JSON_ELEM_END() \
  { \
    ret = OB_AI_FUNC_PARAM_INVALID; \
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_INVALID, elem.first.length(), elem.first.ptr()); \
    LOG_WARN("unknown json key param", K(ret), K(elem.first)); \
  }

int ObAiModelEndpointInfo::parse_from_json_base(common::ObArenaAllocator &allocator, const ObString &name, const ObIJsonBase &params_jbase)
{
  int ret = OB_SUCCESS;
  reset();
  name_ = name;
  if (OB_FAIL(merge_delta_endpoint(allocator, params_jbase))) {
  }
  LOG_INFO("parse from json base", K(ret), K(params_jbase), K(params_jbase.json_type()), K(params_jbase.element_count()));
  return ret;
}

int ObAiModelEndpointInfo::deep_copy(
    common::ObIAllocator &allocator,
    const ObAiModelEndpointInfo &other)
{
  int ret = OB_SUCCESS;
  reset();
  endpoint_id_ = other.endpoint_id_;
  if (OB_FAIL(ob_write_string(allocator, other.name_, name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.scope_, scope_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.ai_model_name_, ai_model_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.url_, url_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.access_key_, access_key_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.provider_, provider_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.request_model_name_, request_model_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.parameters_, parameters_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.request_transform_fn_, request_transform_fn_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.response_transform_fn_, response_transform_fn_))) {
  } else {
    max_concurrency_ = other.max_concurrency_;
  }
  return ret;
}

int ObAiModelEndpointInfo::check_valid() const
{
  int ret = OB_SUCCESS;
  if (name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("name"), "name");
  } else if (scope_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("scope"), "scope");
  } else if (scope_.case_compare(DEFAULT_SCOPE) != 0) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("scope"), "scope");
  } else if (ai_model_name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("ai_model_name"), "ai_model_name");
  } else if (url_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("url"), "url");
  } else if (provider_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("provider"), "provider");
  } else if (!is_valid_provider(provider_)) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("provider"), "provider");
  } else if (access_key_.empty() && is_access_key_required(provider_)) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("access_key"), "access_key");
    LOG_WARN("access_key is empty", K(ret), K(*this));
  } else if (has_max_concurrency()
             && (max_concurrency_ < MIN_MAX_CONCURRENCY || max_concurrency_ > MAX_MAX_CONCURRENCY)) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("max_concurrency"), "max_concurrency");
  } else if (!parameters_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("parameters"), "parameters");
  } else if (!request_transform_fn_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("request_transform_fn"), "request_transform_fn");
  } else if (!response_transform_fn_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("response_transform_fn"), "response_transform_fn");
  }
  return ret;
}

bool ObAiModelEndpointInfo::is_valid_provider(const ObString &provider)
{
  bool is_valid = false;
  for (int i = 0; i < ARRAYSIZEOF(VALID_PROVIDERS); i++) {
    if (provider.case_compare(VALID_PROVIDERS[i].name_) == 0) {
      is_valid = true;
      break;
    }
  }
  return is_valid;
}

bool ObAiModelEndpointInfo::is_access_key_required(const ObString &provider)
{
  // unknown providers are rejected by is_valid_provider, keep them strict here
  bool required = true;
  for (int i = 0; i < ARRAYSIZEOF(VALID_PROVIDERS); i++) {
    if (provider.case_compare(VALID_PROVIDERS[i].name_) == 0) {
      required = VALID_PROVIDERS[i].access_key_required_;
      break;
    }
  }
  return required;
}

int ObAiModelEndpointInfo::parse_max_concurrency_(const ObIJsonBase &value)
{
  int ret = OB_SUCCESS;
  int64_t concurrency = UNSPECIFIED_MAX_CONCURRENCY;
  if (value.json_type() == ObJsonNodeType::J_INT) {
    concurrency = value.get_int();
  } else if (value.json_type() == ObJsonNodeType::J_UINT
             && value.get_uint() <= static_cast<uint64_t>(MAX_MAX_CONCURRENCY)) {
    concurrency = static_cast<int64_t>(value.get_uint());
  } else {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
  }
  if (OB_SUCC(ret) && (concurrency < MIN_MAX_CONCURRENCY || concurrency > MAX_MAX_CONCURRENCY)) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
  }
  if (OB_FAIL(ret)) {
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("max_concurrency"), "max_concurrency");
    LOG_WARN("invalid max_concurrency", K(ret), K(value.json_type()));
  } else {
    max_concurrency_ = concurrency;
  }
  return ret;
}

int ObAiModelEndpointInfo::merge_delta_endpoint(common::ObArenaAllocator &allocator, const ObIJsonBase &delta_jbase)
{
  int ret = OB_SUCCESS;
  JsonObjectIterator iter = delta_jbase.object_iterator();
  bool has_api_key = false;
  while (!iter.end() && OB_SUCC(ret)) {
    ObJsonObjPair elem;
    if (OB_FAIL(iter.get_elem(elem))) {
    } else {
      EXTRACT_JSON_ELEM_STR("scope", scope_)
      EXTRACT_JSON_ELEM_STR("ai_model_name", ai_model_name_)
      EXTRACT_JSON_ELEM_STR("url", url_)
      EXTRACT_JSON_ELEM_STR_WITH_PROCESS("access_key", access_key_, has_api_key = true)
      EXTRACT_JSON_ELEM_STR("provider", provider_)
      EXTRACT_JSON_ELEM_STR("request_model_name", request_model_name_)
      EXTRACT_JSON_ELEM_STR("parameters", parameters_)
      EXTRACT_JSON_ELEM_STR("request_transform_fn", request_transform_fn_)
      EXTRACT_JSON_ELEM_STR("response_transform_fn", response_transform_fn_)
      if (elem.first.case_compare("max_concurrency") == 0) {
        if (OB_ISNULL(elem.second)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(parse_max_concurrency_(*elem.second))) {
        }
      } else
      EXTRACT_JSON_ELEM_END()
    }
    iter.next();
  }

  if (OB_SUCC(ret)) {
    if (has_api_key && !access_key_.empty() && OB_FAIL(encrypt_access_key_(allocator, access_key_, access_key_))) {
      LOG_WARN("failed to encrypt access key", K(ret));
    } else if (OB_FAIL(check_valid())) {
    }
  }

  LOG_INFO("merge delta endpoint", K(ret), K(delta_jbase), K(delta_jbase.json_type()), K(delta_jbase.element_count()));
  return ret;
}

int ObAiModelEndpointInfo::encrypt_access_key_(ObIAllocator &allocator, const ObString &access_key, ObString &encrypted_access_key)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ob_write_string(allocator, access_key, encrypted_access_key))) {
  }
  return ret;
}

int ObAiModelEndpointInfo::decrypt_access_key_(ObIAllocator &allocator, const ObString &encrypted_access_key, ObString &unencrypted_access_key) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ob_write_string(allocator, encrypted_access_key, unencrypted_access_key))) {
  }
  return ret;
}

const char *EndpointType::ENDPOINT_TYPE_STR[] = {
  "DENSE_EMBEDDING",
  "SPARSE_EMBEDDING",
  "COMPLETION",
  "RERANK",
};

EndpointType::TYPE EndpointType::str_to_endpoint_type(const ObString &type_str)
{
  STATIC_ASSERT(static_cast<int64_t>(EndpointType::MAX_TYPE) == ARRAYSIZEOF(ENDPOINT_TYPE_STR) + 1, "endpoint type str len is mismatch");
  EndpointType::TYPE endpoint_type = EndpointType::INVALID_TYPE;
  bool is_found = false;
  for (int i = 1; i < EndpointType::MAX_TYPE && !is_found; i++) {
    if (type_str.case_compare(ENDPOINT_TYPE_STR[i-1]) == 0) {
      endpoint_type = static_cast<EndpointType::TYPE>(i);
      is_found = true;
    }
  }
  return endpoint_type;
}

int ObAiModelEndpointInfo::get_unencrypted_access_key(common::ObIAllocator &allocator, ObString &unencrypted_access_key) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(decrypt_access_key_(allocator, access_key_, unencrypted_access_key))) {
  }
  return ret;
}

int ObAiServiceModelInfo::parse_from_json_base(const ObString &name, const common::ObIJsonBase &params_jbase)
{
  int ret = OB_SUCCESS;
  reset();
  name_ = name;
  JsonObjectIterator iter = params_jbase.object_iterator();
  ObString type_str;
  while (!iter.end() && OB_SUCC(ret)) {
    ObJsonObjPair elem;
    if (OB_FAIL(iter.get_elem(elem))) {
    } else {
      EXTRACT_JSON_ELEM_STR("model_name", model_name_)
      EXTRACT_JSON_ELEM_STR_WITH_PROCESS("type", type_str, type_ = EndpointType::str_to_endpoint_type(type_str))
      EXTRACT_JSON_ELEM_END()
    }
    iter.next();
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(check_valid())) {
    }
  }

  LOG_TRACE("parse from json base", K(ret), K(params_jbase), K(params_jbase.json_type()), K(params_jbase.element_count()));
  return ret;
}

int ObAiServiceModelInfo::check_valid() const
{
  int ret = OB_SUCCESS;
  if (name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("name"), "name");
  } else if (model_name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("model_name"), "model_name");
  } else if (type_ == EndpointType::MAX_TYPE) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("type"), "type");
  } else if (type_ == EndpointType::INVALID_TYPE) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("type"), "type");
  }
  return ret;
}

OB_SERIALIZE_MEMBER(ObAiServiceModelInfo, name_, type_, model_name_);

// ---------------------------------------------------------------------------
// AI column shared definitions. 作者：FDE WorkMate
// ---------------------------------------------------------------------------
int AiColumnUtil::gen_hidden_column_name(ObIAllocator &allocator,
                                         const ObString &column_name,
                                         const HiddenKind kind,
                                         ObString &hidden_name)
{
  int ret = OB_SUCCESS;
  static const char *SUFFIX[HIDDEN_KIND_COUNT] = {"state", "request", "spec"};
  ObSqlString buf;
  if (column_name.empty() || kind < HIDDEN_STATE || kind >= HIDDEN_KIND_COUNT) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(buf.append_fmt("__ai_%.*s_%s", column_name.length(), column_name.ptr(),
                                    SUFFIX[kind]))) {
  } else if (OB_FAIL(ob_write_string(allocator, buf.string(), hidden_name))) {
  }
  return ret;
}

const char *AiColumnUtil::row_state_str(const int64_t state)
{
  const char *str = "UNKNOWN";
  switch (state) {
    case AI_ROW_STATE_PENDING: str = "PENDING"; break;
    case AI_ROW_STATE_READY: str = "READY"; break;
    case AI_ROW_STATE_FAILED: str = "FAILED"; break;
    default: break;
  }
  return str;
}

int64_t AiColumnUtil::calc_spec_version(const ObString &normalized_expr)
{
  uint64_t hash = murmurhash64A(normalized_expr.ptr(), normalized_expr.length(), 0);
  int64_t version = static_cast<int64_t>(hash & INT64_MAX);
  return 0 == version ? 1 : version;
}

bool AiColumnUtil::is_hidden_column_name(const ObString &name)
{
  static const char PREFIX[] = "__ai_";
  static const char *SUFFIX[HIDDEN_KIND_COUNT] = {"_state", "_request", "_spec"};
  const int64_t prefix_len = sizeof(PREFIX) - 1;
  bool bret = false;
  if (name.length() > prefix_len && 0 == strncasecmp(name.ptr(), PREFIX, prefix_len)) {
    for (int64_t i = 0; !bret && i < HIDDEN_KIND_COUNT; ++i) {
      const int64_t len = strlen(SUFFIX[i]);
      bret = name.length() > prefix_len + len
             && 0 == strncasecmp(name.ptr() + name.length() - len, SUFFIX[i], len);
    }
  }
  return bret;
}

int AiColumnUtil::split_call_args(const ObString &call_text,
                                  ObString &func_name,
                                  ObIArray<ObString> &args)
{
  int ret = OB_SUCCESS;
  const char *p = call_text.ptr();
  const int64_t len = call_text.length();
  int64_t open_pos = -1;
  args.reset();
  func_name.reset();
  for (int64_t i = 0; open_pos < 0 && i < len; ++i) {
    if ('(' == p[i]) {
      open_pos = i;
    }
  }
  if (open_pos <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    func_name = ObString(open_pos, p).trim();
    int64_t depth = 0;
    char quote = 0;
    int64_t arg_begin = open_pos + 1;
    int64_t close_pos = -1;
    for (int64_t i = open_pos + 1; OB_SUCC(ret) && close_pos < 0 && i < len; ++i) {
      const char ch = p[i];
      if (0 != quote) {
        if ('\\' == ch && '`' != quote) {
          ++i;  // skip the escaped character
        } else if (ch == quote) {
          if (i + 1 < len && p[i + 1] == quote) {
            ++i;  // doubled quote inside a literal
          } else {
            quote = 0;
          }
        }
      } else if ('\'' == ch || '"' == ch || '`' == ch) {
        quote = ch;
      } else if ('(' == ch) {
        ++depth;
      } else if (depth > 0) {
        if (')' == ch) {
          --depth;  // commas and parentheses of nested calls belong to the argument
        }
      } else if (')' == ch || ',' == ch) {
        ObString arg = ObString(i - arg_begin, p + arg_begin).trim();
        if (arg.empty() && !(')' == ch && args.empty())) {
          ret = OB_INVALID_ARGUMENT;
        } else if (!arg.empty() && OB_FAIL(args.push_back(arg))) {
        } else if (')' == ch) {
          close_pos = i;
        } else {
          arg_begin = i + 1;
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (close_pos < 0 || 0 != quote || func_name.empty()
               || !ObString(len - close_pos - 1, p + close_pos + 1).trim().empty()) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

const char *AiColumnUtil::type_prompt(const ObObjType type)
{
  const char *prompt = "";
  switch (ob_obj_type_class(type)) {
    case ObJsonTC:
      prompt = "Return exactly one valid JSON value and nothing else, without code fences.";
      break;
    case ObIntTC:
    case ObUIntTC:
      prompt = "Return only one base-10 integer and nothing else.";
      break;
    case ObFloatTC:
    case ObDoubleTC:
    case ObNumberTC:
    case ObDecimalIntTC:
      prompt = "Return only one decimal number and nothing else.";
      break;
    case ObDateTC:
    case ObMySQLDateTC:
      prompt = "Return only one date formatted as YYYY-MM-DD and nothing else.";
      break;
    case ObDateTimeTC:
    case ObMySQLDateTimeTC:
      prompt = "Return only one datetime formatted as YYYY-MM-DD HH:MM:SS and nothing else.";
      break;
    default:
      break;
  }
  return prompt;
}

int AiColumnUtil::build_normalized_expr(ObIAllocator &allocator,
                                        const ObString &func_name,
                                        const ObIArray<ObString> &args,
                                        const ObObjType result_type,
                                        ObString &normalized_expr)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  const char *prompt = type_prompt(result_type);
  const bool add_prompt = 0 == func_name.case_compare(FUNC_AI_COMPLETE) && '\0' != prompt[0];
  if (func_name.empty() || args.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(buf.append_fmt("%.*s(", func_name.length(), func_name.ptr()))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < args.count(); ++i) {
    if (add_prompt && 2 == i) {
      // keep caller supplied options, they win over the generated instruction
      if (OB_FAIL(buf.append_fmt(", JSON_MERGE_PATCH('{\"%s\": \"%s\"}', %.*s)",
                                 SYSTEM_PROMPT_KEY, prompt,
                                 args.at(i).length(), args.at(i).ptr()))) {
      }
    } else if (OB_FAIL(buf.append_fmt("%s%.*s", 0 == i ? "" : ", ",
                                      args.at(i).length(), args.at(i).ptr()))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (add_prompt && 2 == args.count()
             && OB_FAIL(buf.append_fmt(", '{\"%s\": \"%s\"}'", SYSTEM_PROMPT_KEY, prompt))) {
  } else if (OB_FAIL(buf.append(")"))) {
  } else if (OB_FAIL(ob_write_string(allocator, buf.string(), normalized_expr))) {
  }
  return ret;
}

int AiColumnUtil::replace_status_pseudo_column(ObIAllocator &allocator,
                                               const ObString &text,
                                               const ObString &replacement,
                                               ObString &result)
{
  int ret = OB_SUCCESS;
  static const char KEYWORD[] = "status";
  const int64_t kw_len = sizeof(KEYWORD) - 1;
  ObSqlString buf;
  const char *p = text.ptr();
  const int64_t len = text.length();
  char quote = 0;
  int64_t copied = 0;
  auto is_ident_char = [](const char ch) {
    return isalnum(static_cast<unsigned char>(ch)) || '_' == ch || '$' == ch
           || static_cast<unsigned char>(ch) >= 0x80;
  };
  for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
    const char ch = p[i];
    if (0 != quote) {
      if ('\\' == ch && '`' != quote) {
        ++i;
      } else if (ch == quote) {
        if (i + 1 < len && p[i + 1] == quote) {
          ++i;
        } else {
          quote = 0;
        }
      }
    } else if ('\'' == ch || '"' == ch || '`' == ch) {
      quote = ch;
    } else if (i + kw_len <= len && 0 == strncasecmp(p + i, KEYWORD, kw_len)
               && (0 == i || (!is_ident_char(p[i - 1]) && '.' != p[i - 1]))
               && (i + kw_len == len || (!is_ident_char(p[i + kw_len]) && '.' != p[i + kw_len]))) {
      if (OB_FAIL(buf.append(ObString(i - copied, p + copied)))) {
      } else if (OB_FAIL(buf.append(replacement))) {
      } else {
        i += kw_len - 1;
        copied = i + 1;
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (copied < len && OB_FAIL(buf.append(ObString(len - copied, p + copied)))) {
  } else if (OB_FAIL(ob_write_string(allocator, buf.string(), result))) {
  }
  return ret;
}

int AiColumnUtil::load_column_definition(ObISQLClient &sql_client,
                                         ObIAllocator &allocator,
                                         const uint64_t table_id,
                                         const uint64_t column_id,
                                         ObString &original_expr,
                                         ObString &sync_mode,
                                         int64_t &max_retries)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  // HEX keeps the longtext readable as a plain varchar through the inner connection
  if (OB_FAIL(sql.append_fmt("SELECT HEX(original_expr) AS expr_hex, sync_mode, max_retries "
                             "FROM %s.%s WHERE table_id = %lu AND column_id = %lu",
                             OB_SYS_DATABASE_NAME, "__all_ai_column", table_id, column_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      ObString expr_hex;
      ObString mode;
      if (OB_FAIL(sql_client.read(res, sql.ptr()))) {
        LOG_WARN("fail to read ai column definition", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_ENTRY_NOT_EXIST;
        }
      } else {
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "expr_hex", expr_hex);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "sync_mode", mode);
        EXTRACT_INT_FIELD_MYSQL(*result, "max_retries", max_retries, int64_t);
        char *buf = NULL;
        const int64_t len = expr_hex.length() / 2;
        if (OB_FAIL(ret)) {
        } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(len + 1)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          auto nibble = [](const char ch) -> int {
            return ch >= '0' && ch <= '9' ? ch - '0'
                 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                 : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : 0;
          };
          for (int64_t i = 0; i < len; ++i) {
            buf[i] = static_cast<char>((nibble(expr_hex.ptr()[2 * i]) << 4)
                                       | nibble(expr_hex.ptr()[2 * i + 1]));
          }
          original_expr.assign_ptr(buf, static_cast<int32_t>(len));
          ret = ob_write_string(allocator, mode, sync_mode);
        }
      }
    }
  }
  return ret;
}

void AiColumnMeta::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  state_column_id_ = OB_INVALID_ID;
  request_column_id_ = OB_INVALID_ID;
  result_spec_column_id_ = OB_INVALID_ID;
  MEMSET(dependency_column_ids_, 0, sizeof(dependency_column_ids_));
  dependency_count_ = 0;
  max_retries_ = 0;
  spec_version_ = 0;
}

bool AiColumnMeta::depends_on(const uint64_t column_id) const
{
  bool bret = false;
  for (int64_t i = 0; !bret && i < dependency_count_; ++i) {
    bret = dependency_column_ids_[i] == column_id;
  }
  return bret;
}

AiColumnMetaCache &AiColumnMetaCache::get_instance()
{
  static AiColumnMetaCache instance;
  return instance;
}

int AiColumnMetaCache::get(ObISQLClient &sql_client,
                           const uint64_t table_id,
                           const int64_t schema_version,
                           ObIArray<AiColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  bool found = false;
  metas.reset();
  {
    ObSpinLockGuard guard(lock_);
    for (int64_t i = 0; OB_SUCC(ret) && !found && i < entries_.count(); ++i) {
      if (entries_.at(i).table_id_ == table_id && entries_.at(i).schema_version_ == schema_version) {
        found = true;
        ret = metas.assign(entries_.at(i).metas_);
      }
    }
  }
  if (OB_SUCC(ret) && !found) {
    if (OB_FAIL(load(sql_client, table_id, metas))) {
      LOG_WARN("fail to load ai column meta", K(ret), K(table_id));
    } else {
      ObSpinLockGuard guard(lock_);
      int64_t slot = -1;
      for (int64_t i = 0; slot < 0 && i < entries_.count(); ++i) {
        if (entries_.at(i).table_id_ == table_id) {
          slot = i;
        }
      }
      if (slot < 0 && entries_.count() < MAX_ENTRY_COUNT) {
        Entry entry;
        if (OB_SUCC(entries_.push_back(entry))) {
          slot = entries_.count() - 1;
        }
      } else if (slot < 0) {
        slot = next_victim_++ % MAX_ENTRY_COUNT;
      }
      if (slot >= 0) {
        Entry &entry = entries_.at(slot);
        entry.table_id_ = table_id;
        entry.schema_version_ = schema_version;
        if (OB_SUCCESS != entry.metas_.assign(metas)) {
          entry.table_id_ = OB_INVALID_ID;  // never keep a partial entry
        }
      }
      ret = OB_SUCCESS;  // caching is best effort
    }
  }
  return ret;
}

int AiColumnMetaCache::load(ObISQLClient &sql_client,
                            const uint64_t table_id,
                            ObIArray<AiColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  metas.reset();
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    common::sqlclient::ObMySQLResult *result = NULL;
    if (OB_FAIL(sql.append_fmt("SELECT column_id, state_column_id, request_column_id, "
                               "result_spec_column_id, dependency_column_ids, max_retries, "
                               "spec_version FROM %s.%s WHERE table_id = %lu ORDER BY column_id",
                               OB_SYS_DATABASE_NAME, "__all_ai_column", table_id))) {
    } else if (OB_FAIL(sql_client.read(res, sql.ptr()))) {
      LOG_WARN("fail to read ai column meta", K(ret), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    }
    while (OB_SUCC(ret) && OB_SUCC(result->next())) {
      AiColumnMeta meta;
      ObString deps;
      meta.table_id_ = table_id;
      EXTRACT_INT_FIELD_MYSQL(*result, "column_id", meta.column_id_, uint64_t);
      EXTRACT_INT_FIELD_MYSQL(*result, "state_column_id", meta.state_column_id_, uint64_t);
      EXTRACT_INT_FIELD_MYSQL(*result, "request_column_id", meta.request_column_id_, uint64_t);
      EXTRACT_INT_FIELD_MYSQL(*result, "result_spec_column_id", meta.result_spec_column_id_, uint64_t);
      EXTRACT_INT_FIELD_MYSQL(*result, "max_retries", meta.max_retries_, int64_t);
      EXTRACT_INT_FIELD_MYSQL(*result, "spec_version", meta.spec_version_, int64_t);
      EXTRACT_VARCHAR_FIELD_MYSQL(*result, "dependency_column_ids", deps);
      while (OB_SUCC(ret) && !deps.empty()) {
        ObString item = deps.split_on(',');
        if (item.empty()) {
          item = deps;
          deps.reset();
        }
        item = item.trim();
        if (item.empty()) {
        } else if (meta.dependency_count_ >= AiColumnMeta::MAX_DEPENDENCY_COUNT) {
          ret = OB_SIZE_OVERFLOW;
        } else {
          uint64_t id = 0;
          for (int64_t k = 0; OB_SUCC(ret) && k < item.length(); ++k) {
            if (item.ptr()[k] < '0' || item.ptr()[k] > '9') {
              ret = OB_INVALID_DATA;
            } else {
              id = id * 10 + (item.ptr()[k] - '0');
            }
          }
          if (OB_SUCC(ret)) {
            meta.dependency_column_ids_[meta.dependency_count_++] = id;
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(metas.push_back(meta))) {
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

void AiColumnDef::reset()
{
  column_name_.reset();
  state_column_name_.reset();
  request_column_name_.reset();
  result_spec_column_name_.reset();
  function_name_.reset();
  model_name_.reset();
  original_expr_.reset();
  normalized_expr_.reset();
  dependency_names_.reset();
  sync_mode_.reset();
  max_retries_ = AiColumnUtil::DEFAULT_MAX_RETRIES;
  result_type_.reset();
  spec_version_ = 0;
}

int AiColumnDef::gen_meta_insert_sql(const uint64_t table_id,
                                     const uint64_t column_id,
                                     const uint64_t state_column_id,
                                     const uint64_t request_column_id,
                                     const uint64_t result_spec_column_id,
                                     const ObString &dependency_column_ids,
                                     ObSqlString &sql) const
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (OB_INVALID_ID == table_id || OB_INVALID_ID == column_id || !is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.append_fmt("INSERT INTO %s.%s (table_id, column_id, column_name, "
             "state_column_id, request_column_id, result_spec_column_id, function_name, "
             "model_name, original_expr, normalized_expr, dependency_column_ids, sync_mode, "
             "max_retries, spec_version, result_type) VALUES (%lu, %lu, ",
             OB_SYS_DATABASE_NAME, "__all_ai_column", table_id, column_id))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(column_name_, sql))) {
  } else if (OB_FAIL(sql.append_fmt(", %lu, %lu, %lu, ", state_column_id, request_column_id,
                                    result_spec_column_id))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(function_name_, sql))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(model_name_, sql))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(original_expr_, sql))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(normalized_expr_, sql))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(dependency_column_ids, sql))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(sync_mode_, sql))) {
  } else if (OB_FAIL(sql.append_fmt(", %ld, %ld, ", max_retries_, spec_version_))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(result_type_, sql))) {
  } else if (OB_FAIL(sql.append(")"))) {
  }
  return ret;
}

OB_SERIALIZE_MEMBER(AiColumnDef, column_name_, state_column_name_, request_column_name_,
                    result_spec_column_name_, function_name_, model_name_, original_expr_,
                    normalized_expr_, dependency_names_, sync_mode_, max_retries_, result_type_,
                    spec_version_);

} // namespace share
} // namespace oceanbase
