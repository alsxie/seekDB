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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/file_sql/file_scanner.h"

#include <algorithm>
#include <cstdio>
#include <glob.h>
#include <sys/stat.h>

#include "lib/oblog/ob_log_module.h"
#include "share/ob_errno.h"
#include "sql/engine/basic/file_sql/parquet_reader.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace file_sql
{

FileFormat get_table_function_format(const ObString &func_name)
{
  FileFormat format = FileFormat::INVALID;
  if (0 == func_name.case_compare("read_csv") || 0 == func_name.case_compare("read_csv_auto")) {
    format = FileFormat::CSV;
  } else if (0 == func_name.case_compare("read_json") || 0 == func_name.case_compare("read_json_auto")
             || 0 == func_name.case_compare("read_ndjson")
             || 0 == func_name.case_compare("read_ndjson_auto")) {
    format = FileFormat::JSONL;
  } else if (0 == func_name.case_compare("read_parquet")
             || 0 == func_name.case_compare("parquet_scan")) {
    format = FileFormat::PARQUET;
  } else if (0 == func_name.case_compare("glob")) {
    format = FileFormat::GLOB;
  }
  return format;
}

static int report_unsupported_option(const ObString &func_name, const FileOption &opt,
                                     const char *reason)
{
  int ret = OB_NOT_SUPPORTED;
  FORWARD_USER_ERROR_MSG(ret, "%.*s option '%.*s' %s", func_name.length(), func_name.ptr(),
                         opt.name_.length(), opt.name_.ptr(), reason);
  return ret;
}

static int get_bool_option(const ObString &func_name, const FileOption &opt, bool &value)
{
  int ret = OB_SUCCESS;
  if (FileOptionValueType::BOOL == opt.type_ || FileOptionValueType::NONE == opt.type_) {
    value = FileOptionValueType::NONE == opt.type_ ? true : opt.bool_;
  } else if (FileOptionValueType::INT == opt.type_ && (0 == opt.int_ || 1 == opt.int_)) {
    value = 1 == opt.int_;
  } else if ((FileOptionValueType::STRING == opt.type_ || FileOptionValueType::IDENT == opt.type_)
             && TextValueParser::parse_bool(opt.str_.ptr(), opt.str_.length(), value)) {
  } else {
    ret = report_unsupported_option(func_name, opt, "expects a boolean value");
  }
  return ret;
}

static int get_char_option(const ObString &func_name, const FileOption &opt, const bool allow_empty,
                           char &value)
{
  int ret = OB_SUCCESS;
  if (FileOptionValueType::STRING != opt.type_) {
    ret = report_unsupported_option(func_name, opt, "expects a string value");
  } else if (1 == opt.str_.length() && '\n' != opt.str_[0] && '\r' != opt.str_[0]) {
    value = opt.str_[0];
  } else if (2 == opt.str_.length() && '\\' == opt.str_[0] && 't' == opt.str_[1]) {
    value = '\t';
  } else if (allow_empty && opt.str_.empty()) {
    value = '\0';
  } else {
    ret = report_unsupported_option(func_name, opt, "only supports a single character");
  }
  return ret;
}

static int check_sample_size(const ObString &func_name, const FileOption &opt)
{
  int ret = OB_SUCCESS;
  if (FileOptionValueType::INT != opt.type_ || -1 != opt.int_) {
    ret = report_unsupported_option(func_name, opt, "only supports -1 (scan the whole file)");
  }
  return ret;
}

static int get_size_option(const ObString &func_name, const FileOption &opt, int64_t &value)
{
  int ret = OB_SUCCESS;
  if (FileOptionValueType::INT != opt.type_ || opt.int_ <= 0
      || opt.int_ > FILE_SQL_MAX_LINE_SIZE_LIMIT) {
    ret = report_unsupported_option(func_name, opt, "expects a positive size within 64MB");
  } else {
    value = opt.int_;
  }
  return ret;
}

int parse_table_function_options(const ObString &func_name, const ObIArray<FileOption> &options,
                                 FileScanDesc &desc)
{
  int ret = OB_SUCCESS;
  desc.format_ = get_table_function_format(func_name);
  desc.func_name_ = func_name;
  if (FileFormat::INVALID == desc.format_) {
    ret = OB_NOT_SUPPORTED;
    FORWARD_USER_ERROR_MSG(ret, "table function '%.*s' is not supported",
                           func_name.length(), func_name.ptr());
  }
  bool has_escape = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < options.count(); ++i) {
    const FileOption &opt = options.at(i);
    const ObString &n = opt.name_;
    for (int64_t j = 0; OB_SUCC(ret) && j < i; ++j) {
      if (0 == options.at(j).name_.case_compare(n)) {
        ret = report_unsupported_option(func_name, opt, "is specified more than once");
      }
    }
    if (OB_FAIL(ret)) {
    } else if (FileFormat::CSV == desc.format_) {
      if (0 == n.case_compare("header")) {
        ret = get_bool_option(func_name, opt, desc.csv_.header_);
      } else if (0 == n.case_compare("delim") || 0 == n.case_compare("sep")
                 || 0 == n.case_compare("delimiter")) {
        ret = get_char_option(func_name, opt, false, desc.csv_.delim_);
      } else if (0 == n.case_compare("quote")) {
        ret = get_char_option(func_name, opt, true, desc.csv_.quote_);
      } else if (0 == n.case_compare("escape")) {
        has_escape = true;
        ret = get_char_option(func_name, opt, true, desc.csv_.escape_);
      } else if (0 == n.case_compare("nullstr") || 0 == n.case_compare("null")) {
        if (FileOptionValueType::STRING != opt.type_) {
          ret = report_unsupported_option(func_name, opt, "expects a string value");
        } else {
          desc.csv_.has_null_str_ = true;
          desc.csv_.null_str_ = opt.str_;
        }
      } else if (0 == n.case_compare("sample_size")) {
        ret = check_sample_size(func_name, opt);
      } else if (0 == n.case_compare("ignore_errors")) {
        ret = get_bool_option(func_name, opt, desc.csv_.ignore_errors_);
      } else if (0 == n.case_compare("max_line_size")) {
        ret = get_size_option(func_name, opt, desc.max_line_size_);
      } else if (0 == n.case_compare("auto_detect")) {
        bool v = false;
        if (OB_SUCC(get_bool_option(func_name, opt, v)) && !v) {
          ret = report_unsupported_option(func_name, opt, "only supports true");
        }
      } else {
        ret = report_unsupported_option(func_name, opt, "is not supported");
      }
    } else if (FileFormat::JSONL == desc.format_) {
      if (0 == n.case_compare("format")) {
        if (FileOptionValueType::STRING != opt.type_ && FileOptionValueType::IDENT != opt.type_) {
          ret = report_unsupported_option(func_name, opt, "expects a string value");
        } else if (0 != opt.str_.case_compare("newline_delimited")
                   && 0 != opt.str_.case_compare("nd")
                   && 0 != opt.str_.case_compare("auto")) {
          ret = report_unsupported_option(func_name, opt,
                                          "only supports 'newline_delimited' or 'auto'");
        }
      } else if (0 == n.case_compare("sample_size")) {
        ret = check_sample_size(func_name, opt);
      } else if (0 == n.case_compare("ignore_errors")) {
        ret = get_bool_option(func_name, opt, desc.csv_.ignore_errors_);
      } else if (0 == n.case_compare("maximum_object_size") || 0 == n.case_compare("max_line_size")) {
        ret = get_size_option(func_name, opt, desc.max_line_size_);
      } else if (0 == n.case_compare("auto_detect")) {
        bool v = false;
        if (OB_SUCC(get_bool_option(func_name, opt, v)) && !v) {
          ret = report_unsupported_option(func_name, opt, "only supports true");
        }
      } else {
        ret = report_unsupported_option(func_name, opt, "is not supported");
      }
    } else {
      ret = report_unsupported_option(func_name, opt, "is not supported");
    }
  }
  if (OB_SUCC(ret) && FileFormat::CSV == desc.format_) {
    if (!has_escape) {
      desc.csv_.escape_ = desc.csv_.quote_;
    }
    if (desc.csv_.delim_ == desc.csv_.quote_
        || ('\0' != desc.csv_.escape_ && desc.csv_.delim_ == desc.csv_.escape_)) {
      ret = OB_NOT_SUPPORTED;
      FORWARD_USER_ERROR_MSG(ret, "%.*s delimiter must differ from quote and escape",
                             func_name.length(), func_name.ptr());
    }
  }
  return ret;
}

int report_file_data_error(const ObString &func_name, const char *msg)
{
  int ret = OB_INVALID_DATA;
  FORWARD_USER_ERROR_MSG(ret, "%.*s: %s", func_name.length(), func_name.ptr(), msg);
  return ret;
}

int make_unique_column_name(ObIAllocator &allocator, const ObString &base, const int64_t idx,
                            const ObIArray<FileColumnDesc> &columns, ObString &name)
{
  int ret = OB_SUCCESS;
  char buf[FILE_SQL_MAX_COLUMN_NAME_LEN + 32];
  int64_t base_len = 0;
  if (base.empty()) {
    base_len = snprintf(buf, sizeof(buf), "column%ld", idx);
  } else {
    // truncate on a UTF-8 character boundary
    base_len = MIN(base.length(), FILE_SQL_MAX_COLUMN_NAME_LEN);
    while (base_len > 0 && base_len < base.length()
           && (static_cast<unsigned char>(base[base_len]) & 0xC0) == 0x80) {
      --base_len;
    }
    MEMCPY(buf, base.ptr(), base_len);
  }
  int64_t len = base_len;
  bool unique = false;
  for (int64_t suffix = 1; OB_SUCC(ret) && !unique; ++suffix) {
    if (suffix > 1) {
      len = base_len + snprintf(buf + base_len, sizeof(buf) - base_len, "__%ld", suffix);
    }
    unique = true;
    const ObString cand(static_cast<int32_t>(len), buf);
    for (int64_t i = 0; unique && i < columns.count(); ++i) {
      unique = 0 != columns.at(i).name_.case_compare(cand);
    }
    if (suffix > FILE_SQL_MAX_COLUMNS + 1) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, ObString(static_cast<int32_t>(len), buf), name))) {
    LOG_WARN("failed to copy column name", K(ret));
  }
  return ret;
}

static void finish_column(const ColumnInferState &state, FileColumnDesc &col)
{
  col.type_ = state.get_type();
  col.nullable_ = state.has_null() || !state.has_value();
  col.scale_ = FileColumnType::DATETIME == col.type_ ? state.get_datetime_scale() : 0;
  col.max_length_ = MIN(MAX(1, state.get_max_char_length()), FILE_SQL_MAX_VARCHAR_LENGTH);
}

// ------------------------------------------------------------------- CSV

bool CsvScanner::is_null_field(const CsvRecordReader &reader, const CsvOptions &opt, const int64_t idx)
{
  bool is_null = false;
  if (!reader.is_field_quoted(idx)) {
    const ObString v = reader.get_field(idx);
    is_null = v.empty() || (opt.has_null_str_ && v == opt.null_str_);
  }
  return is_null;
}

int CsvScanner::next_valid_record(CsvRecordReader &reader, const ObString &func_name,
                                  const bool ignore_errors, const int64_t expected_fields,
                                  bool &has_record)
{
  int ret = OB_SUCCESS;
  bool done = false;
  char msg[FILE_SQL_ERROR_MSG_LEN];
  while (OB_SUCC(ret) && !done) {
    ret = reader.next_record(has_record);
    if (OB_SUCCESS == ret && has_record && expected_fields >= 0
        && reader.get_field_count() != expected_fields) {
      snprintf(msg, sizeof(msg), "line %ld: expected %ld columns but found %ld",
               reader.get_record_line(), expected_fields, reader.get_field_count());
      ret = OB_INVALID_DATA;
    } else if (OB_INVALID_DATA == ret) {
      snprintf(msg, sizeof(msg), "%s", reader.get_error_msg());
    }
    if (OB_SUCC(ret)) {
      done = true;
    } else if (OB_INVALID_DATA == ret && ignore_errors && reader.is_recoverable()
               && '\0' != msg[0]) {
      ret = OB_SUCCESS;  // skip the malformed record
    } else if (OB_INVALID_DATA == ret && ignore_errors && !reader.is_recoverable()) {
      ret = OB_SUCCESS;
      has_record = false;  // nothing can follow an unterminated quote
      done = true;
    } else if (OB_INVALID_DATA == ret && '\0' != msg[0]) {
      ret = report_file_data_error(func_name, msg);
    }
  }
  return ret;
}

static int infer_csv_schema(ObIAllocator &allocator, FileScanDesc &desc)
{
  int ret = OB_SUCCESS;
  CsvRecordReader reader;
  ObSEArray<ColumnInferState, 16> states;
  bool has_record = false;
  int64_t row_count = 0;
  if (OB_FAIL(reader.open(desc.path_, nullptr, desc.csv_, desc.max_line_size_))) {
    LOG_WARN("failed to open csv", K(ret));
  } else if (OB_FAIL(CsvScanner::next_valid_record(reader, desc.func_name_, false, -1, has_record))) {
    LOG_WARN("failed to read first record", K(ret));
  } else if (!has_record) {
    ret = report_file_data_error(desc.func_name_, "file is empty, cannot infer columns");
  } else {
    desc.fingerprint_ = reader.get_fingerprint();
    desc.columns_.reuse();
    const int64_t col_cnt = reader.get_field_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
      FileColumnDesc col;
      const ObString base = desc.csv_.header_ ? reader.get_field(i) : ObString();
      col.source_idx_ = static_cast<int32_t>(i);
      col.source_ = FileValueSource::TEXT;
      if (desc.csv_.header_ && !TextValueParser::is_valid_utf8(base.ptr(), base.length())) {
        ret = report_file_data_error(desc.func_name_, "line 1: invalid UTF-8 in header");
      } else if (OB_FAIL(make_unique_column_name(allocator, base, i, desc.columns_, col.name_))) {
      } else if (OB_FAIL(desc.columns_.push_back(col))) {
      } else if (OB_FAIL(states.push_back(ColumnInferState()))) {
      }
    }
    // without header the first record is data
    bool use_current = !desc.csv_.header_;
    while (OB_SUCC(ret)) {
      if (!use_current) {
        if (OB_FAIL(CsvScanner::next_valid_record(reader, desc.func_name_, desc.csv_.ignore_errors_,
                                                  col_cnt, has_record))) {
          LOG_WARN("failed to read record", K(ret));
        } else if (!has_record) {
          break;
        }
      }
      use_current = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
        if (CsvScanner::is_null_field(reader, desc.csv_, i)) {
          states.at(i).add_null();
        } else {
          const ObString v = reader.get_field(i);
          states.at(i).add_text(v.ptr(), v.length());
        }
      }
      ++row_count;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
      finish_column(states.at(i), desc.columns_.at(i));
    }
    desc.row_count_ = row_count;
  }
  reader.close();
  return ret;
}

int CsvScanner::open(const FileScanDesc &desc, const ObIArray<bool> &projected)
{
  int ret = OB_SUCCESS;
  desc_ = &desc;
  if (OB_FAIL(projected_.assign(projected))) {
  } else if (OB_FAIL(reader_.open(desc.path_, &desc.fingerprint_, desc.csv_, desc.max_line_size_))) {
    LOG_WARN("failed to open csv", K(ret));
  } else if (OB_FAIL(skip_header())) {
  }
  return ret;
}

int CsvScanner::skip_header()
{
  int ret = OB_SUCCESS;
  bool has_record = false;
  if (desc_->csv_.header_
      && OB_FAIL(next_valid_record(reader_, desc_->func_name_, false, -1, has_record))) {
    LOG_WARN("failed to skip header", K(ret));
  }
  return ret;
}

int CsvScanner::rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(reader_.rewind())) {
    LOG_WARN("failed to rewind", K(ret));
  } else if (OB_FAIL(skip_header())) {
  }
  return ret;
}

int CsvScanner::get_next_row(ObIArray<FileValue> &row)
{
  int ret = OB_SUCCESS;
  bool has_record = false;
  const int64_t col_cnt = desc_->columns_.count();
  if (OB_FAIL(next_valid_record(reader_, desc_->func_name_, desc_->csv_.ignore_errors_, col_cnt,
                                has_record))) {
  } else if (!has_record) {
    ret = OB_ITER_END;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
      FileValue &v = row.at(i);
      const FileColumnDesc &col = desc_->columns_.at(i);
      v.set_null();
      if (!projected_.at(i) || is_null_field(reader_, desc_->csv_, col.source_idx_)) {
      } else {
        const ObString text = reader_.get_field(col.source_idx_);
        bool ok = false;
        if (OB_FAIL(convert_text_value(col.type_, text.ptr(), text.length(), v, ok))) {
        } else if (!ok) {
          char msg[FILE_SQL_ERROR_MSG_LEN];
          snprintf(msg, sizeof(msg), "line %ld: value of column '%.*s' does not match type %s",
                   reader_.get_record_line(), col.name_.length(), col.name_.ptr(),
                   get_file_column_type_name(col.type_));
          ret = report_file_data_error(desc_->func_name_, msg);
        }
      }
    }
  }
  return ret;
}

// ------------------------------------------------------------------ JSONL

int JsonlScanner::next_valid_object(JsonlReader &reader, const ObString &func_name,
                                    const bool ignore_errors, bool &has_object)
{
  int ret = OB_SUCCESS;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    ret = reader.next_object(has_object);
    if (OB_SUCC(ret)) {
      done = true;
    } else if (OB_INVALID_DATA == ret && '\0' != reader.get_error_msg()[0]) {
      if (ignore_errors) {
        ret = OB_SUCCESS;
      } else {
        ret = report_file_data_error(func_name, reader.get_error_msg());
      }
    }
  }
  return ret;
}

int JsonlScanner::convert_json_value(const FileColumnType type, const JsonMember &member,
                                     FileValue &value, bool &ok)
{
  int ret = OB_SUCCESS;
  ok = true;
  switch (member.type_) {
    case JsonScalarType::JSON_NULL: {
      value.set_null();
      break;
    }
    case JsonScalarType::JSON_BOOL: {
      if (FileColumnType::BOOLEAN == type) {
        value.set_int(member.bool_ ? 1 : 0);
      } else if (FileColumnType::VARCHAR == type) {
        value.set_string(member.bool_ ? ObString("true") : ObString("false"));
      } else {
        ok = false;
      }
      break;
    }
    case JsonScalarType::JSON_NUMBER: {
      if (FileColumnType::BIGINT == type || FileColumnType::DOUBLE == type
          || FileColumnType::VARCHAR == type) {
        ret = convert_text_value(type, member.text_.ptr(), member.text_.length(), value, ok);
      } else {
        ok = false;
      }
      break;
    }
    case JsonScalarType::JSON_STRING: {
      if (FileColumnType::DATE == type || FileColumnType::DATETIME == type
          || FileColumnType::VARCHAR == type) {
        ret = convert_text_value(type, member.text_.ptr(), member.text_.length(), value, ok);
      } else {
        ok = false;
      }
      break;
    }
    default: {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

static int infer_jsonl_schema(ObIAllocator &allocator, FileScanDesc &desc)
{
  int ret = OB_SUCCESS;
  JsonlReader reader;
  ObSEArray<ColumnInferState, 16> states;
  hash::ObHashMap<ObString, int64_t, hash::NoPthreadDefendMode> key_map;
  bool has_object = false;
  int64_t row_count = 0;
  desc.columns_.reuse();
  if (OB_FAIL(key_map.create(256, "FileSqlJson", "FileSqlJson"))) {
    LOG_WARN("failed to create map", K(ret));
  } else if (OB_FAIL(reader.open(desc.path_, nullptr, desc.max_line_size_))) {
    LOG_WARN("failed to open jsonl", K(ret));
  } else {
    desc.fingerprint_ = reader.get_fingerprint();
  }
  while (OB_SUCC(ret)) {
    if (OB_FAIL(JsonlScanner::next_valid_object(reader, desc.func_name_, desc.csv_.ignore_errors_,
                                                has_object))) {
    } else if (!has_object) {
      break;
    } else {
      const ObIArray<JsonMember> &members = reader.get_members();
      // keys absent from this object count as NULL
      ObSEArray<bool, 32> seen;
      for (int64_t i = 0; OB_SUCC(ret) && i < states.count(); ++i) {
        ret = seen.push_back(false);
      }
      for (int64_t j = 0; OB_SUCC(ret) && j < members.count(); ++j) {
        const JsonMember &m = members.at(j);
        int64_t idx = -1;
        if (OB_FAIL(key_map.get_refactored(m.key_, idx))) {
          if (OB_HASH_NOT_EXIST == ret) {
            ret = OB_SUCCESS;
            FileColumnDesc col;
            ColumnInferState state;
            idx = desc.columns_.count();
            if (idx >= FILE_SQL_MAX_COLUMNS) {
              ret = report_file_data_error(desc.func_name_, "too many columns");
            } else if (OB_FAIL(ob_write_string(allocator, m.key_, col.source_name_))) {
            } else if (OB_FAIL(make_unique_column_name(allocator, m.key_, idx, desc.columns_,
                                                       col.name_))) {
            } else if (OB_FAIL(key_map.set_refactored(col.source_name_, idx))) {
            } else {
              col.source_idx_ = static_cast<int32_t>(idx);
              if (row_count > 0) {
                state.add_missing();  // absent from earlier objects
              }
              if (OB_FAIL(desc.columns_.push_back(col))) {
              } else if (OB_FAIL(states.push_back(state))) {
              } else if (OB_FAIL(seen.push_back(false))) {
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          ColumnInferState &s = states.at(idx);
          seen.at(idx) = true;
          switch (m.type_) {
            case JsonScalarType::JSON_NULL: s.add_null(); break;
            case JsonScalarType::JSON_BOOL: s.add_json_bool(); break;
            case JsonScalarType::JSON_NUMBER: s.add_json_number(m.text_.ptr(), m.text_.length()); break;
            case JsonScalarType::JSON_STRING: s.add_json_string(m.text_.ptr(), m.text_.length()); break;
            default: break;
          }
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < seen.count(); ++i) {
        if (!seen.at(i)) {
          states.at(i).add_missing();
        }
      }
      ++row_count;
    }
  }
  if (OB_SUCC(ret)) {
    if (desc.columns_.empty()) {
      ret = report_file_data_error(desc.func_name_, "no JSON object found, cannot infer columns");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < desc.columns_.count(); ++i) {
      finish_column(states.at(i), desc.columns_.at(i));
    }
    desc.row_count_ = row_count;
  }
  reader.close();
  return ret;
}

int JsonlScanner::open(const FileScanDesc &desc, const ObIArray<bool> &projected)
{
  int ret = OB_SUCCESS;
  desc_ = &desc;
  if (OB_FAIL(key_map_.create(MAX(16, desc.columns_.count() * 2), "FileSqlJson", "FileSqlJson"))) {
    LOG_WARN("failed to create map", K(ret));
  } else {
    map_created_ = true;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < desc.columns_.count(); ++i) {
    if (projected.at(i) && OB_FAIL(key_map_.set_refactored(desc.columns_.at(i).source_name_, i))) {
      LOG_WARN("failed to set map", K(ret));
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(reader_.open(desc.path_, &desc.fingerprint_, desc.max_line_size_))) {
    LOG_WARN("failed to open jsonl", K(ret));
  }
  return ret;
}

int JsonlScanner::rescan()
{
  return reader_.rewind();
}

void JsonlScanner::close()
{
  reader_.close();
  if (map_created_) {
    key_map_.destroy();
    map_created_ = false;
  }
}

int JsonlScanner::get_next_row(ObIArray<FileValue> &row)
{
  int ret = OB_SUCCESS;
  bool has_object = false;
  if (OB_FAIL(next_valid_object(reader_, desc_->func_name_, desc_->csv_.ignore_errors_, has_object))) {
  } else if (!has_object) {
    ret = OB_ITER_END;
  } else {
    for (int64_t i = 0; i < row.count(); ++i) {
      row.at(i).set_null();
    }
    const ObIArray<JsonMember> &members = reader_.get_members();
    for (int64_t j = 0; OB_SUCC(ret) && j < members.count(); ++j) {
      const JsonMember &m = members.at(j);
      int64_t idx = -1;
      bool ok = false;
      if (OB_FAIL(key_map_.get_refactored(m.key_, idx))) {
        if (OB_HASH_NOT_EXIST == ret) {
          ret = OB_SUCCESS;  // not projected
        }
      } else if (OB_FAIL(convert_json_value(desc_->columns_.at(idx).type_, m, row.at(idx), ok))) {
      } else if (!ok) {
        char msg[FILE_SQL_ERROR_MSG_LEN];
        snprintf(msg, sizeof(msg), "line %ld: value of key '%.*s' does not match type %s",
                 reader_.get_object_line(), m.key_.length(), m.key_.ptr(),
                 get_file_column_type_name(desc_->columns_.at(idx).type_));
        ret = report_file_data_error(desc_->func_name_, msg);
      }
    }
  }
  return ret;
}

// ------------------------------------------------------------------- glob

int GlobScanner::expand(ObIAllocator &allocator, const ObString &pattern,
                        const ObString &secure_file_priv, ObIArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  char pattern_buf[PATH_MAX + 1];
  glob_t g;
  MEMSET(&g, 0, sizeof(g));
  if (pattern.empty() || pattern.length() > PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
    FORWARD_USER_ERROR_MSG(ret, "invalid glob pattern");
  } else {
    MEMCPY(pattern_buf, pattern.ptr(), pattern.length());
    pattern_buf[pattern.length()] = '\0';
    const int rc = ::glob(pattern_buf, GLOB_NOSORT, nullptr, &g);
    if (GLOB_NOMATCH == rc) {
    } else if (0 != rc) {
      ret = OB_IO_ERROR;
      FORWARD_USER_ERROR_MSG(ret, "glob '%.*s' failed", pattern.length(), pattern.ptr());
    } else {
      char real_buf[PATH_MAX + 1];
      for (size_t i = 0; OB_SUCC(ret) && i < g.gl_pathc; ++i) {
        struct stat st;
        bool allowed = false;
        ObString path;
        if (0 != ::stat(g.gl_pathv[i], &st) || !S_ISREG(st.st_mode)) {
        } else if (NULL == ::realpath(g.gl_pathv[i], real_buf)) {
        } else if (OB_FAIL(check_path_in_secure_dir(ObString(real_buf), secure_file_priv, allowed))) {
        } else if (!allowed) {
        } else if (files.count() >= FILE_SQL_MAX_GLOB_FILES) {
          ret = OB_SIZE_OVERFLOW;
          FORWARD_USER_ERROR_MSG(ret, "glob matches more than %ld files", FILE_SQL_MAX_GLOB_FILES);
        } else if (OB_FAIL(ob_write_string(allocator, ObString(g.gl_pathv[i]), path))) {
        } else if (OB_FAIL(files.push_back(path))) {
        }
      }
    }
    globfree(&g);
  }
  if (OB_SUCC(ret) && files.count() > 1) {
    std::sort(&files.at(0), &files.at(0) + files.count(),
              [](const ObString &a, const ObString &b) { return a.compare(b) < 0; });
  }
  return ret;
}

int GlobScanner::open(const FileScanDesc &desc, const ObIArray<bool> &projected)
{
  UNUSED(projected);
  close();
  return expand(allocator_, desc.path_, desc.secure_file_priv_, files_);
}

int GlobScanner::get_next_row(ObIArray<FileValue> &row)
{
  int ret = OB_SUCCESS;
  if (cur_ >= files_.count()) {
    ret = OB_ITER_END;
  } else {
    row.at(0).set_string(files_.at(cur_++));
  }
  return ret;
}

// ------------------------------------------------------------- dispatching

int infer_file_schema(ObIAllocator &allocator, FileScanDesc &desc)
{
  int ret = OB_SUCCESS;
  switch (desc.format_) {
    case FileFormat::CSV: ret = infer_csv_schema(allocator, desc); break;
    case FileFormat::JSONL: ret = infer_jsonl_schema(allocator, desc); break;
    case FileFormat::PARQUET: ret = infer_parquet_schema(allocator, desc); break;
    case FileFormat::GLOB: {
      FileColumnDesc col;
      col.name_ = ObString("file");
      col.type_ = FileColumnType::VARCHAR;
      col.nullable_ = false;
      col.max_length_ = PATH_MAX;
      col.source_idx_ = 0;
      desc.columns_.reuse();
      ret = desc.columns_.push_back(col);
      break;
    }
    default: {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected format", K(ret), K(desc.format_));
    }
  }
  return ret;
}

int build_file_scan_desc(ObIAllocator &allocator, const ObString &func_name, const ObString &path,
                         const ObIArray<FileOption> &options, const ObString &secure_file_priv,
                         FileScanDesc &desc)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(parse_table_function_options(func_name, options, desc))) {
    LOG_WARN("invalid table function options", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, secure_file_priv, desc.secure_file_priv_))) {
  } else if (FileFormat::GLOB == desc.format_) {
    ObString priv = secure_file_priv;
    priv = priv.trim();
    if (priv.empty() || 0 == priv.case_compare("NULL")) {
      ret = OB_ERR_NO_PRIVILEGE;
      FORWARD_USER_ERROR_MSG(ret, "glob is disabled because secure_file_priv is NULL");
    } else if (OB_FAIL(ob_write_string(allocator, path, desc.path_))) {
    }
  } else if (OB_FAIL(resolve_secure_path(path, secure_file_priv, allocator, desc.path_))) {
    LOG_WARN("path is not allowed", K(ret));
  }
  if (OB_SUCC(ret) && OB_FAIL(infer_file_schema(allocator, desc))) {
    LOG_WARN("failed to infer schema", K(ret));
  }
  return ret;
}

static int print_option_literal(const char *str, const int64_t len, char *buf, const int64_t buf_len,
                                int64_t &pos)
{
  int ret = databuff_printf(buf, buf_len, pos, "'");
  for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
    if ('\'' == str[i]) {
      ret = databuff_printf(buf, buf_len, pos, "''");
    } else if ('\\' == str[i]) {
      ret = databuff_printf(buf, buf_len, pos, "\\\\");
    } else {
      ret = databuff_printf(buf, buf_len, pos, "%c", str[i]);
    }
  }
  if (OB_SUCC(ret)) {
    ret = databuff_printf(buf, buf_len, pos, "'");
  }
  return ret;
}

static int print_char_option(const char *name, const char c, char *buf, const int64_t buf_len,
                             int64_t &pos)
{
  int ret = databuff_printf(buf, buf_len, pos, ", %s = ", name);
  if (OB_SUCC(ret)) {
    ret = print_option_literal(&c, '\0' == c ? 0 : 1, buf, buf_len, pos);
  }
  return ret;
}

int print_table_function_options(const FileScanDesc &desc, char *buf, const int64_t buf_len,
                                 int64_t &pos)
{
  int ret = OB_SUCCESS;
  if (FileFormat::CSV == desc.format_) {
    if (OB_FAIL(databuff_printf(buf, buf_len, pos, ", header = %s",
                                desc.csv_.header_ ? "true" : "false"))) {
    } else if (OB_FAIL(print_char_option("delim", desc.csv_.delim_, buf, buf_len, pos))) {
    } else if (OB_FAIL(print_char_option("quote", desc.csv_.quote_, buf, buf_len, pos))) {
    } else if (OB_FAIL(print_char_option("escape", desc.csv_.escape_, buf, buf_len, pos))) {
    } else if (desc.csv_.has_null_str_) {
      if (OB_FAIL(databuff_printf(buf, buf_len, pos, ", nullstr = "))) {
      } else if (OB_FAIL(print_option_literal(desc.csv_.null_str_.ptr(), desc.csv_.null_str_.length(),
                                              buf, buf_len, pos))) {
      }
    }
  }
  if (OB_SUCC(ret) && (FileFormat::CSV == desc.format_ || FileFormat::JSONL == desc.format_)) {
    if (OB_FAIL(databuff_printf(buf, buf_len, pos, ", ignore_errors = %s, max_line_size = %ld",
                                desc.csv_.ignore_errors_ ? "true" : "false", desc.max_line_size_))) {
    }
  }
  return ret;
}

int create_file_scanner(ObIAllocator &allocator, const FileFormat format, FileScanner *&scanner)
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  scanner = nullptr;
  switch (format) {
    case FileFormat::CSV:
      if (OB_NOT_NULL(buf = allocator.alloc(sizeof(CsvScanner)))) { scanner = new (buf) CsvScanner(); }
      break;
    case FileFormat::JSONL:
      if (OB_NOT_NULL(buf = allocator.alloc(sizeof(JsonlScanner)))) { scanner = new (buf) JsonlScanner(); }
      break;
    case FileFormat::PARQUET:
      if (OB_NOT_NULL(buf = allocator.alloc(sizeof(ParquetScanner)))) { scanner = new (buf) ParquetScanner(); }
      break;
    case FileFormat::GLOB:
      if (OB_NOT_NULL(buf = allocator.alloc(sizeof(GlobScanner)))) { scanner = new (buf) GlobScanner(); }
      break;
    default:
      ret = OB_ERR_UNEXPECTED;
  }
  if (OB_SUCC(ret) && OB_ISNULL(scanner)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate scanner", K(ret));
  }
  return ret;
}

void destroy_file_scanner(ObIAllocator &allocator, FileScanner *&scanner)
{
  if (OB_NOT_NULL(scanner)) {
    scanner->close();
    scanner->~FileScanner();
    allocator.free(scanner);
    scanner = nullptr;
  }
}

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
