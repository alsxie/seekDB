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
// Format independent entry points of file tables:
//   * build_file_scan_desc(): validates the table function call (name,
//     options, path security) and infers the schema over the whole file;
//   * FileScanner: streaming row source used by the executor.

#pragma once

#include "lib/allocator/page_arena.h"
#include "lib/hash/ob_hashmap.h"
#include "sql/engine/basic/file_sql/file_text_reader.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

enum class FileOptionValueType : int8_t
{
  NONE = 0,     // bare option, e.g. COPY ... (header)
  STRING = 1,
  INT = 2,
  BOOL = 3,
  IDENT = 4,    // unquoted identifier, e.g. format csv
};

struct FileOption
{
  FileOption() : name_(), type_(FileOptionValueType::NONE), str_(), int_(0), bool_(false) {}
  TO_STRING_KV(K_(name), K_(type), K_(str), K_(int), K_(bool));
  common::ObString name_;
  FileOptionValueType type_;
  common::ObString str_;
  int64_t int_;
  bool bool_;
};

// Maps a table function name to its format; INVALID when unknown.
FileFormat get_table_function_format(const common::ObString &func_name);

// Validates options of `func_name` and fills `desc` (without schema).
int parse_table_function_options(const common::ObString &func_name,
                                 const common::ObIArray<FileOption> &options,
                                 FileScanDesc &desc);

// Full resolution: options, secure path and schema inference. All strings of
// `desc` are allocated from `allocator`.
int build_file_scan_desc(common::ObIAllocator &allocator,
                         const common::ObString &func_name,
                         const common::ObString &path,
                         const common::ObIArray<FileOption> &options,
                         const common::ObString &secure_file_priv,
                         FileScanDesc &desc);

// Schema inference over the whole file for an already validated `desc`.
int infer_file_schema(common::ObIAllocator &allocator, FileScanDesc &desc);

// Prints the effective options as `, name = value ...` so that a printed
// statement (e.g. a view definition) resolves to the same scan.
int print_table_function_options(const FileScanDesc &desc, char *buf, const int64_t buf_len,
                                 int64_t &pos);

// Builds a unique (case insensitive) column name from `base`; empty names
// become column<idx>, duplicates get a "__<n>" suffix.
int make_unique_column_name(common::ObIAllocator &allocator, const common::ObString &base,
                            const int64_t idx, const common::ObIArray<FileColumnDesc> &columns,
                            common::ObString &name);

// Reports a data error of a table function with a user visible message.
int report_file_data_error(const common::ObString &func_name, const char *msg);

class FileScanner
{
public:
  virtual ~FileScanner() {}
  // `projected[i]` tells whether column i is consumed; others may stay NULL.
  virtual int open(const FileScanDesc &desc, const common::ObIArray<bool> &projected) = 0;
  virtual int rescan() = 0;
  // Fills `row`, which the caller sizes to one value per column of the
  // descriptor. Returns OB_ITER_END at the end.
  // Strings stay valid until the next call.
  virtual int get_next_row(common::ObIArray<FileValue> &row) = 0;
  virtual void close() = 0;
};

int create_file_scanner(common::ObIAllocator &allocator, const FileFormat format,
                        FileScanner *&scanner);
void destroy_file_scanner(common::ObIAllocator &allocator, FileScanner *&scanner);

class CsvScanner : public FileScanner
{
public:
  CsvScanner() : desc_(nullptr), reader_(), projected_() {}
  virtual ~CsvScanner() { close(); }
  virtual int open(const FileScanDesc &desc, const common::ObIArray<bool> &projected) override;
  virtual int rescan() override;
  virtual int get_next_row(common::ObIArray<FileValue> &row) override;
  virtual void close() override { reader_.close(); }
  // Reads the next record accepted by the shared validity rules of inference
  // and scan: structural errors fail, or are skipped with ignore_errors.
  static int next_valid_record(CsvRecordReader &reader, const common::ObString &func_name,
                               const bool ignore_errors, const int64_t expected_fields,
                               bool &has_record);
  static bool is_null_field(const CsvRecordReader &reader, const CsvOptions &opt, const int64_t idx);
private:
  int skip_header();
  const FileScanDesc *desc_;
  CsvRecordReader reader_;
  common::ObSEArray<bool, 16> projected_;
};

class JsonlScanner : public FileScanner
{
public:
  JsonlScanner() : desc_(nullptr), reader_(), key_map_(), map_created_(false) {}
  virtual ~JsonlScanner() { close(); }
  virtual int open(const FileScanDesc &desc, const common::ObIArray<bool> &projected) override;
  virtual int rescan() override;
  virtual int get_next_row(common::ObIArray<FileValue> &row) override;
  virtual void close() override;
  static int next_valid_object(JsonlReader &reader, const common::ObString &func_name,
                               const bool ignore_errors, bool &has_object);
  static int convert_json_value(const FileColumnType type, const JsonMember &member,
                                FileValue &value, bool &ok);
private:
  const FileScanDesc *desc_;
  JsonlReader reader_;
  common::hash::ObHashMap<common::ObString, int64_t, common::hash::NoPthreadDefendMode> key_map_;
  bool map_created_;
};

class GlobScanner : public FileScanner
{
public:
  GlobScanner() : allocator_("FileSqlGlob"), files_(), cur_(0) {}
  virtual ~GlobScanner() { close(); }
  virtual int open(const FileScanDesc &desc, const common::ObIArray<bool> &projected) override;
  virtual int rescan() override { cur_ = 0; return common::OB_SUCCESS; }
  virtual int get_next_row(common::ObIArray<FileValue> &row) override;
  virtual void close() override { files_.reset(); allocator_.reset(); cur_ = 0; }
  // Expands `pattern` into sorted regular files inside secure_file_priv.
  static int expand(common::ObIAllocator &allocator, const common::ObString &pattern,
                    const common::ObString &secure_file_priv,
                    common::ObIArray<common::ObString> &files);
private:
  common::ObArenaAllocator allocator_;
  common::ObSEArray<common::ObString, 16> files_;
  int64_t cur_;
};

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
