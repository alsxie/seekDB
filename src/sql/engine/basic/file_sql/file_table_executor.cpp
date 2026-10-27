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

#include "sql/engine/basic/file_sql/file_table_executor.h"
#include "sql/engine/basic/ob_json_table_op.h"
#include "lib/charset/ob_charset.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace file_sql
{

FileTableExecutor::FileTableExecutor()
  : allocator_("FileSqlExec"), desc_(), scanner_(nullptr), row_(), out_exprs_(), projected_(),
    inited_(false), opened_(false)
{
}

int FileTableExecutor::init(const ObJsonTableSpec &spec)
{
  int ret = OB_SUCCESS;
  ObString desc_buf;
  if (OB_UNLIKELY(inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(spec.namespace_def_.count() < 1)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file scan desc missing", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator_, spec.namespace_def_.at(0), desc_buf))) {
    LOG_WARN("failed to copy desc", K(ret));
  } else if (OB_FAIL(desc_.deserialize(desc_buf))) {
    LOG_WARN("failed to deserialize file scan desc", K(ret));
  } else if (OB_UNLIKELY(spec.cols_def_.count() != desc_.columns_.count() + 1)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("column count mismatch", K(ret), K(spec.cols_def_.count()), K(desc_.columns_.count()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < desc_.columns_.count(); ++i) {
    const ObJtColInfo *info = spec.cols_def_.at(i + 1);
    ObExpr *expr = nullptr;
    if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (info->output_column_idx_ >= 0) {
      if (OB_UNLIKELY(info->output_column_idx_ >= spec.column_exprs_.count())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid output column idx", K(ret), K(*info));
      } else {
        expr = spec.column_exprs_.at(info->output_column_idx_);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(out_exprs_.push_back(expr))) {
    } else if (OB_FAIL(projected_.push_back(nullptr != expr))) {
    } else if (OB_FAIL(row_.push_back(FileValue()))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(create_file_scanner(allocator_, desc_.format_, scanner_))) {
    LOG_WARN("failed to create scanner", K(ret));
  }
  if (OB_SUCC(ret)) {
    inited_ = true;
  }
  return ret;
}

int FileTableExecutor::open()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_) || OB_ISNULL(scanner_)) {
    ret = OB_NOT_INIT;
  } else if (opened_) {
    ret = scanner_->rescan();
  } else if (OB_FAIL(scanner_->open(desc_, projected_))) {
    LOG_WARN("failed to open file scanner", K(ret), K(desc_.path_));
  } else {
    opened_ = true;
  }
  return ret;
}

int FileTableExecutor::rescan()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_) || OB_ISNULL(scanner_)) {
    ret = OB_NOT_INIT;
  } else if (!opened_) {
    ret = open();
  } else if (OB_FAIL(scanner_->rescan())) {
    LOG_WARN("failed to rescan file", K(ret), K(desc_.path_));
  }
  return ret;
}

int FileTableExecutor::get_next_row(ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!opened_) || OB_ISNULL(scanner_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(scanner_->get_next_row(row_))) {
    if (OB_ITER_END != ret) {
      LOG_WARN("failed to scan file", K(ret), K(desc_.path_));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < out_exprs_.count(); ++i) {
    if (nullptr != out_exprs_.at(i) && OB_FAIL(fill_datum(i, eval_ctx))) {
      LOG_WARN("failed to fill datum", K(ret), K(i));
    }
  }
  return ret;
}

int FileTableExecutor::fill_datum(const int64_t col_idx, ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  ObExpr *expr = out_exprs_.at(col_idx);
  const FileValue &value = row_.at(col_idx);
  const FileColumnDesc &col = desc_.columns_.at(col_idx);
  ObDatum &datum = expr->locate_datum_for_write(eval_ctx);
  if (value.is_null_) {
    datum.set_null();
  } else {
    switch (col.type_) {
      case FileColumnType::BOOLEAN:
      case FileColumnType::BIGINT:
        datum.set_int(value.int_);
        break;
      case FileColumnType::DOUBLE:
        datum.set_double(value.double_);
        break;
      case FileColumnType::DATE:
        datum.set_date(static_cast<int32_t>(value.int_));
        break;
      case FileColumnType::DATETIME:
        datum.set_datetime(value.int_);
        break;
      case FileColumnType::VARCHAR:
      case FileColumnType::NULL_TYPE: {
        int64_t well_len = 0;
        if (OB_FAIL(ObCharset::well_formed_len(CS_TYPE_UTF8MB4_BIN, value.str_.ptr(),
                                               value.str_.length(), well_len))
            || well_len != value.str_.length()) {
          ret = OB_INVALID_DATA;
          LOG_WARN("invalid utf8 string in file", K(ret), K(col.name_));
          FORWARD_USER_ERROR_MSG(ret, "%.*s: column '%.*s' contains invalid UTF-8 data",
                                 desc_.func_name_.length(), desc_.func_name_.ptr(),
                                 col.name_.length(), col.name_.ptr());
        } else {
          datum.set_string(value.str_.ptr(), value.str_.length());
        }
        break;
      }
      default:
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected column type", K(ret), K(col));
    }
  }
  if (OB_SUCC(ret)) {
    expr->set_evaluated_projected(eval_ctx);
  }
  return ret;
}

void FileTableExecutor::close()
{
  if (OB_NOT_NULL(scanner_)) {
    scanner_->close();
  }
  opened_ = false;
}

void FileTableExecutor::destroy()
{
  close();
  destroy_file_scanner(allocator_, scanner_);
  row_.reset();
  out_exprs_.reset();
  projected_.reset();
  desc_.columns_.reset();
  allocator_.reset();
  inited_ = false;
}

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
