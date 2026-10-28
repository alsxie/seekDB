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
// Unit tests for the file SQL scan utilities: value parsing, type inference,
// CSV / JSONL / Parquet readers, path security and the COPY CSV writer.

#include <gtest/gtest.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "sql/engine/basic/file_sql/file_sql_common.h"
#include "sql/engine/basic/file_sql/file_text_reader.h"
#include "sql/engine/basic/file_sql/file_scanner.h"
#include "sql/engine/basic/file_sql/parquet_reader.h"
#include "sql/engine/basic/file_sql/file_copy_writer.h"

using namespace oceanbase::common;
using namespace oceanbase::sql::file_sql;
using namespace oceanbase::sql::file_sql::parquet;

namespace
{

// Parquet file written by arrow-rs: one nullable INT32 column `c1` holding
// 0..6 (7 rows), dictionary encoded and snappy compressed.
const char *SIMPLE_PARQUET_B64 =
  "UEFSMRUGFTwVQFwVDhUAFQ4VABUEFQARHDYAKAQGAAAAGAQAAAAAAAAAA38cbAAAAAABAAAAAgAAAAMAAAAEAAAABQAAAAYAAAAm"
  "lAEcFQIZJQYAGRgCYzEVAhYOFogBFowBJgg8NgAoBAYAAAAYBAAAAAAAAAAVBBksSARyb290FQIAFQIlAhgCYzEAFg4ZHBkcJpQB"
  "HBUCGSUGABkYAmMxFQIWDhaIARaMASYIPDYAKAQGAAAAGAQAAAAAAAAAFowBFg4AGRwYDEFSUk9XOnNjaGVtYRi8AS8vLy8vNFFB"
  "QUFBUUFBQUFBQUFLQUE0QURBQUxBQVFBQ2dBQUFCUUFBQUFBQUFBQkJBQUtBQXdBQUFBSUFBUUFDZ0FBQUFnQUFBQUlBQUFBQUFB"
  "QUFBRUFBQUFVQUFBQUVBQVVBQkFBRGdBUEFBUUFBQUFJQUJBQUFBQVlBQUFBSUFBQUFBQUFBUUljQUFBQUNBQU1BQVFBQ3dBSUFB"
  "QUFJQUFBQUFBQUFBRUFBQUFBQWdBQUFHTXhBQUE9ABgsQXJyb3cyIC0gTmF0aXZlIFJ1c3QgaW1wbGVtZW50YXRpb24gb2YgQXJy"
  "b3cATgEAAFBBUjE=";

std::string base64_decode(const char *in)
{
  std::string out;
  int val = 0;
  int bits = -8;
  for (const char *p = in; *p != '\0' && *p != '='; ++p) {
    const char c = *p;
    int d = -1;
    if (c >= 'A' && c <= 'Z') { d = c - 'A'; }
    else if (c >= 'a' && c <= 'z') { d = c - 'a' + 26; }
    else if (c >= '0' && c <= '9') { d = c - '0' + 52; }
    else if (c == '+') { d = 62; }
    else if (c == '/') { d = 63; }
    if (d < 0) { continue; }
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

// Owned copy of a scanned value; FileValue strings only live until the next row.
struct TestValue
{
  bool is_null_;
  int64_t int_;
  double double_;
  std::string str_;
  ObString str() const { return ObString(static_cast<int32_t>(str_.size()), str_.data()); }
};
typedef std::vector<std::vector<TestValue>> TestRows;

class FileScanUtilsTest : public ::testing::Test
{
protected:
  virtual void SetUp() override
  {
    char tmpl[] = "/tmp/file_sql_ut_XXXXXX";
    ASSERT_NE(nullptr, mkdtemp(tmpl));
    char real[PATH_MAX];
    ASSERT_NE(nullptr, realpath(tmpl, real));
    dir_ = real;
  }
  virtual void TearDown() override
  {
    std::string cmd = "rm -rf '" + dir_ + "'";
    ASSERT_EQ(0, system(cmd.c_str()));
  }
  std::string write_file(const char *name, const std::string &content)
  {
    std::string path = dir_ + "/" + name;
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    EXPECT_GE(fd, 0);
    EXPECT_EQ(static_cast<ssize_t>(content.size()), ::write(fd, content.data(), content.size()));
    ::close(fd);
    return path;
  }
  std::string read_file(const std::string &path)
  {
    std::string out;
    int fd = ::open(path.c_str(), O_RDONLY);
    EXPECT_GE(fd, 0);
    char buf[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
      out.append(buf, n);
    }
    ::close(fd);
    return out;
  }
  ObString str(const std::string &s) { return ObString(static_cast<int32_t>(s.size()), s.data()); }
  int build_desc(const char *func, const std::string &path, const ObIArray<FileOption> &options,
                 FileScanDesc &desc)
  {
    return build_file_scan_desc(allocator_, ObString(func), str(path), options, str(dir_), desc);
  }
  int scan_all(const FileScanDesc &desc, TestRows &rows)
  {
    int ret = OB_SUCCESS;
    FileScanner *scanner = nullptr;
    ObSEArray<bool, 8> projected;
    ObSEArray<FileValue, 8> row;
    for (int64_t i = 0; OB_SUCC(ret) && i < desc.columns_.count(); ++i) {
      if (OB_FAIL(projected.push_back(true))) {
      } else if (OB_FAIL(row.push_back(FileValue()))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(create_file_scanner(allocator_, desc.format_, scanner))) {
    } else if (OB_FAIL(scanner->open(desc, projected))) {
    } else {
      while (OB_SUCC(ret)) {
        if (OB_FAIL(scanner->get_next_row(row))) {
        } else {
          std::vector<TestValue> copy;
          for (int64_t i = 0; i < row.count(); ++i) {
            const FileValue &v = row.at(i);
            TestValue t;
            t.is_null_ = v.is_null_;
            t.int_ = v.int_;
            t.double_ = v.double_;
            t.str_.assign(v.str_.ptr() == nullptr ? "" : v.str_.ptr(), v.str_.length());
            copy.push_back(t);
          }
          rows.push_back(copy);
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
    if (nullptr != scanner) {
      scanner->close();
      destroy_file_scanner(allocator_, scanner);
    }
    return ret;
  }
  std::string dir_;
  ObArenaAllocator allocator_;
};

TEST_F(FileScanUtilsTest, text_value_parser)
{
  bool b = false;
  EXPECT_TRUE(TextValueParser::parse_bool("true", 4, b));
  EXPECT_TRUE(b);
  EXPECT_TRUE(TextValueParser::parse_bool("FALSE", 5, b));
  EXPECT_FALSE(b);
  EXPECT_FALSE(TextValueParser::parse_bool("yes", 3, b));

  int64_t i = 0;
  bool overflow = false;
  EXPECT_TRUE(TextValueParser::parse_bigint("-42", 3, i, overflow));
  EXPECT_EQ(-42, i);
  EXPECT_TRUE(TextValueParser::parse_bigint("9223372036854775807", 19, i, overflow));
  EXPECT_EQ(INT64_MAX, i);
  // syntactically an integer, but flagged as out of range
  EXPECT_TRUE(TextValueParser::parse_bigint("9223372036854775808", 19, i, overflow));
  EXPECT_TRUE(overflow);
  EXPECT_FALSE(TextValueParser::parse_bigint("12a", 3, i, overflow));
  EXPECT_FALSE(TextValueParser::parse_bigint("", 0, i, overflow));

  double d = 0;
  EXPECT_TRUE(TextValueParser::parse_double("98.5", 4, d));
  EXPECT_DOUBLE_EQ(98.5, d);
  EXPECT_TRUE(TextValueParser::parse_double("-1e3", 4, d));
  EXPECT_DOUBLE_EQ(-1000.0, d);
  EXPECT_FALSE(TextValueParser::parse_double("1.2.3", 5, d));

  int32_t days = 0;
  EXPECT_TRUE(TextValueParser::parse_date("1970-01-02", 10, days));
  EXPECT_EQ(1, days);
  EXPECT_TRUE(TextValueParser::parse_date("2024-02-29", 10, days));
  EXPECT_FALSE(TextValueParser::parse_date("2026-02-30", 10, days));
  EXPECT_FALSE(TextValueParser::parse_date("2026-13-01", 10, days));

  int64_t usec = 0;
  int16_t scale = 0;
  EXPECT_TRUE(TextValueParser::parse_datetime("1970-01-01 00:00:01", 19, usec, scale));
  EXPECT_EQ(1000000, usec);
  EXPECT_EQ(0, scale);
  EXPECT_TRUE(TextValueParser::parse_datetime("1970-01-01 00:00:01.25", 22, usec, scale));
  EXPECT_EQ(1250000, usec);
  EXPECT_EQ(2, scale);
  EXPECT_FALSE(TextValueParser::parse_datetime("1970-01-01 24:00:00", 19, usec, scale));

  EXPECT_TRUE(TextValueParser::is_valid_utf8("\xe4\xb8\xad", 3));
  EXPECT_FALSE(TextValueParser::is_valid_utf8("\xff\xfe", 2));
  EXPECT_FALSE(TextValueParser::is_valid_utf8("\xc0\xaf", 2));  // overlong
  EXPECT_EQ(2, TextValueParser::utf8_char_length("\xe4\xb8\xad" "a", 4));
}

TEST_F(FileScanUtilsTest, column_type_inference)
{
  ColumnInferState s1;
  s1.add_text("1", 1);
  s1.add_text("2", 1);
  EXPECT_EQ(FileColumnType::BIGINT, s1.get_type());

  ColumnInferState s2;
  s2.add_text("1", 1);
  s2.add_text("2.5", 3);
  EXPECT_EQ(FileColumnType::DOUBLE, s2.get_type());

  ColumnInferState s3;
  s3.add_text("true", 4);
  s3.add_text("false", 5);
  s3.add_null();
  EXPECT_EQ(FileColumnType::BOOLEAN, s3.get_type());
  EXPECT_TRUE(s3.has_null());

  ColumnInferState s4;
  s4.add_text("2026-08-01", 10);
  EXPECT_EQ(FileColumnType::DATE, s4.get_type());
  s4.add_text("2026-08-01 10:11:12", 19);
  EXPECT_EQ(FileColumnType::DATETIME, s4.get_type());
  s4.add_text("hello", 5);
  EXPECT_EQ(FileColumnType::VARCHAR, s4.get_type());

  ColumnInferState s5;
  s5.add_null();
  EXPECT_FALSE(s5.has_value());

  // An integer beyond BIGINT widens to DOUBLE instead of being truncated.
  ColumnInferState s6;
  s6.add_text("9223372036854775808", 19);
  EXPECT_EQ(FileColumnType::DOUBLE, s6.get_type());
}

TEST_F(FileScanUtilsTest, csv_record_reader)
{
  std::string path = write_file("r.csv", std::string("\xEF\xBB\xBF") +
                                "a,b,c\r\n"
                                "1,\"x,y\",\"say \"\"hi\"\"\"\n"
                                "\n"
                                "2,\"multi\nline\",\n");
  CsvOptions opt;
  CsvRecordReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(str(path), nullptr, opt, FILE_SQL_DEFAULT_MAX_LINE_SIZE));
  bool has = false;
  ASSERT_EQ(OB_SUCCESS, reader.next_record(has));
  ASSERT_TRUE(has);
  ASSERT_EQ(3, reader.get_field_count());
  EXPECT_EQ(ObString("a"), reader.get_field(0));  // BOM skipped
  EXPECT_EQ(ObString("c"), reader.get_field(2));  // CR stripped
  ASSERT_EQ(OB_SUCCESS, reader.next_record(has));
  ASSERT_TRUE(has);
  EXPECT_EQ(ObString("x,y"), reader.get_field(1));
  EXPECT_EQ(ObString("say \"hi\""), reader.get_field(2));
  EXPECT_TRUE(reader.is_field_quoted(1));
  ASSERT_EQ(OB_SUCCESS, reader.next_record(has));  // blank line skipped
  ASSERT_TRUE(has);
  EXPECT_EQ(ObString("multi\nline"), reader.get_field(1));
  EXPECT_EQ(ObString(""), reader.get_field(2));
  EXPECT_FALSE(reader.is_field_quoted(2));
  ASSERT_EQ(OB_SUCCESS, reader.next_record(has));
  EXPECT_FALSE(has);
  // rewind restarts from the first record
  ASSERT_EQ(OB_SUCCESS, reader.rewind());
  ASSERT_EQ(OB_SUCCESS, reader.next_record(has));
  ASSERT_TRUE(has);
  EXPECT_EQ(ObString("a"), reader.get_field(0));
  reader.close();

  std::string bad = write_file("bad.csv", "a\n\"open\n");
  CsvRecordReader bad_reader;
  ASSERT_EQ(OB_SUCCESS, bad_reader.open(str(bad), nullptr, opt, FILE_SQL_DEFAULT_MAX_LINE_SIZE));
  ASSERT_EQ(OB_SUCCESS, bad_reader.next_record(has));
  EXPECT_NE(OB_SUCCESS, bad_reader.next_record(has));
  EXPECT_NE(nullptr, bad_reader.get_error_msg());
}

TEST_F(FileScanUtilsTest, csv_line_size_limit)
{
  std::string path = write_file("long.csv", "a\n" + std::string(4096, 'x') + "\n");
  CsvOptions opt;
  CsvRecordReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(str(path), nullptr, opt, 1024));
  bool has = false;
  ASSERT_EQ(OB_SUCCESS, reader.next_record(has));
  EXPECT_NE(OB_SUCCESS, reader.next_record(has));
}

TEST_F(FileScanUtilsTest, jsonl_reader)
{
  std::string path = write_file("r.jsonl",
                                "{\"a\": 1, \"b\": \"x\\u00e9\\n\", \"c\": true, \"d\": null}\n"
                                "  \n"
                                "{\"a\": -2.5e1, \"a\": 3}\n");
  JsonlReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(str(path), nullptr, FILE_SQL_DEFAULT_MAX_LINE_SIZE));
  bool has = false;
  ASSERT_EQ(OB_SUCCESS, reader.next_object(has));
  ASSERT_TRUE(has);
  const ObIArray<JsonMember> &m = reader.get_members();
  ASSERT_EQ(4, m.count());
  EXPECT_EQ(ObString("a"), m.at(0).key_);
  EXPECT_EQ(JsonScalarType::JSON_NUMBER, m.at(0).type_);
  EXPECT_EQ(ObString("x\xc3\xa9\n"), m.at(1).text_);
  EXPECT_EQ(JsonScalarType::JSON_BOOL, m.at(2).type_);
  EXPECT_TRUE(m.at(2).bool_);
  EXPECT_EQ(JsonScalarType::JSON_NULL, m.at(3).type_);
  ASSERT_EQ(OB_SUCCESS, reader.next_object(has));
  ASSERT_TRUE(has);
  ASSERT_EQ(OB_SUCCESS, reader.next_object(has));
  EXPECT_FALSE(has);

  const char *bad_lines[] = {"[1,2]\n", "{\"a\": {\"b\": 1}}\n", "{\"a\": [1]}\n",
                             "{\"a\": 1,}\n", "{\"a\": tru}\n", "{\"a\": \"\\ud800\"}\n",
                             "{\"a\": 1} x\n"};
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(bad_lines) / sizeof(bad_lines[0])); ++i) {
    std::string bad = write_file("bad.jsonl", bad_lines[i]);
    JsonlReader bad_reader;
    ASSERT_EQ(OB_SUCCESS, bad_reader.open(str(bad), nullptr, FILE_SQL_DEFAULT_MAX_LINE_SIZE));
    EXPECT_NE(OB_SUCCESS, bad_reader.next_object(has)) << bad_lines[i];
  }
}

TEST_F(FileScanUtilsTest, csv_schema_and_scan)
{
  std::string path = write_file("basic.csv",
                                "id,name,score,active,day,,id\n"
                                "1,Alice,98.5,true,2026-08-01,x,101\n"
                                "2,Bob,87,false,,y,102\n");
  ObSEArray<FileOption, 4> options;
  FileScanDesc desc;
  ASSERT_EQ(OB_SUCCESS, build_desc("read_csv", path, options, desc));
  ASSERT_EQ(FileFormat::CSV, desc.format_);
  ASSERT_EQ(7, desc.columns_.count());
  EXPECT_EQ(ObString("id"), desc.columns_.at(0).name_);
  EXPECT_EQ(FileColumnType::BIGINT, desc.columns_.at(0).type_);
  EXPECT_FALSE(desc.columns_.at(0).nullable_);
  EXPECT_EQ(FileColumnType::VARCHAR, desc.columns_.at(1).type_);
  EXPECT_EQ(FileColumnType::DOUBLE, desc.columns_.at(2).type_);
  EXPECT_EQ(FileColumnType::BOOLEAN, desc.columns_.at(3).type_);
  EXPECT_EQ(FileColumnType::DATE, desc.columns_.at(4).type_);
  EXPECT_TRUE(desc.columns_.at(4).nullable_);
  EXPECT_EQ(ObString("column5"), desc.columns_.at(5).name_);
  EXPECT_EQ(ObString("id__2"), desc.columns_.at(6).name_);

  // descriptor survives a serialize round trip
  ObString buf;
  ASSERT_EQ(OB_SUCCESS, desc.serialize(allocator_, buf));
  FileScanDesc copy;
  ASSERT_EQ(OB_SUCCESS, copy.deserialize(buf));
  ASSERT_EQ(desc.columns_.count(), copy.columns_.count());
  EXPECT_EQ(desc.path_, copy.path_);
  EXPECT_TRUE(desc.fingerprint_ == copy.fingerprint_);
  int64_t idx = -1;
  ASSERT_EQ(OB_SUCCESS, copy.find_column(ObString("ID__2"), idx));
  EXPECT_EQ(6, idx);

  TestRows rows;
  ASSERT_EQ(OB_SUCCESS, scan_all(copy, rows));
  ASSERT_EQ(2, static_cast<int64_t>(rows.size()));
  EXPECT_EQ(1, rows.at(0).at(0).int_);
  EXPECT_EQ(ObString("Alice"), rows.at(0).at(1).str());
  EXPECT_DOUBLE_EQ(98.5, rows.at(0).at(2).double_);
  EXPECT_EQ(1, rows.at(0).at(3).int_);
  EXPECT_TRUE(rows.at(1).at(4).is_null_);
  EXPECT_EQ(102, rows.at(1).at(6).int_);

  // a changed file is detected instead of returning stale results
  usleep(10 * 1000);
  write_file("basic.csv", "id\n1\n");
  TestRows stale;
  EXPECT_NE(OB_SUCCESS, scan_all(copy, stale));
}

TEST_F(FileScanUtilsTest, csv_options)
{
  std::string path = write_file("semi.csv", "1;N/A\n2;'a;b'\n");
  ObSEArray<FileOption, 4> options;
  FileOption opt;
  opt.name_ = ObString("header");
  opt.type_ = FileOptionValueType::BOOL;
  opt.bool_ = false;
  ASSERT_EQ(OB_SUCCESS, options.push_back(opt));
  opt.name_ = ObString("delim");
  opt.type_ = FileOptionValueType::STRING;
  opt.str_ = ObString(";");
  ASSERT_EQ(OB_SUCCESS, options.push_back(opt));
  opt.name_ = ObString("quote");
  opt.str_ = ObString("'");
  ASSERT_EQ(OB_SUCCESS, options.push_back(opt));
  opt.name_ = ObString("nullstr");
  opt.str_ = ObString("N/A");
  ASSERT_EQ(OB_SUCCESS, options.push_back(opt));
  FileScanDesc desc;
  ASSERT_EQ(OB_SUCCESS, build_desc("read_csv_auto", path, options, desc));
  ASSERT_EQ(2, desc.columns_.count());
  EXPECT_EQ(ObString("column0"), desc.columns_.at(0).name_);
  EXPECT_TRUE(desc.columns_.at(1).nullable_);
  TestRows rows;
  ASSERT_EQ(OB_SUCCESS, scan_all(desc, rows));
  ASSERT_EQ(2, static_cast<int64_t>(rows.size()));
  EXPECT_TRUE(rows.at(0).at(1).is_null_);
  EXPECT_EQ(ObString("a;b"), rows.at(1).at(1).str());
}

TEST_F(FileScanUtilsTest, invalid_options)
{
  std::string path = write_file("t.csv", "a\n1\n");
  FileOption opt;
  FileScanDesc desc;
  {
    ObSEArray<FileOption, 4> options;
    opt.name_ = ObString("sample_size");
    opt.type_ = FileOptionValueType::INT;
    opt.int_ = 10;
    ASSERT_EQ(OB_SUCCESS, options.push_back(opt));
    EXPECT_EQ(OB_NOT_SUPPORTED, build_desc("read_csv", path, options, desc));
  }
  {
    ObSEArray<FileOption, 4> options;
    opt.int_ = -1;
    ASSERT_EQ(OB_SUCCESS, options.push_back(opt));
    FileScanDesc ok_desc;
    EXPECT_EQ(OB_SUCCESS, build_desc("read_csv", path, options, ok_desc));
  }
  {
    ObSEArray<FileOption, 4> options;
    opt.name_ = ObString("no_such_option");
    ASSERT_EQ(OB_SUCCESS, options.push_back(opt));
    EXPECT_EQ(OB_NOT_SUPPORTED, build_desc("read_csv", path, options, desc));
  }
  {
    ObSEArray<FileOption, 4> options;
    opt.name_ = ObString("delim");
    opt.type_ = FileOptionValueType::STRING;
    opt.str_ = ObString("ab");
    ASSERT_EQ(OB_SUCCESS, options.push_back(opt));
    EXPECT_NE(OB_SUCCESS, build_desc("read_csv", path, options, desc));
  }
  EXPECT_EQ(FileFormat::INVALID, get_table_function_format(ObString("read_xml")));
  EXPECT_EQ(FileFormat::PARQUET, get_table_function_format(ObString("PARQUET_SCAN")));
  EXPECT_EQ(FileFormat::JSONL, get_table_function_format(ObString("read_ndjson_auto")));
}

TEST_F(FileScanUtilsTest, jsonl_union_schema)
{
  std::string path = write_file("u.jsonl",
                                "{\"id\": 1, \"name\": \"a\"}\n"
                                "{\"id\": 2, \"extra\": 1.5}\n");
  ObSEArray<FileOption, 4> options;
  FileScanDesc desc;
  ASSERT_EQ(OB_SUCCESS, build_desc("read_json", path, options, desc));
  ASSERT_EQ(3, desc.columns_.count());
  EXPECT_EQ(ObString("id"), desc.columns_.at(0).name_);
  EXPECT_FALSE(desc.columns_.at(0).nullable_);
  EXPECT_EQ(ObString("name"), desc.columns_.at(1).name_);
  EXPECT_TRUE(desc.columns_.at(1).nullable_);
  EXPECT_EQ(FileColumnType::DOUBLE, desc.columns_.at(2).type_);
  TestRows rows;
  ASSERT_EQ(OB_SUCCESS, scan_all(desc, rows));
  ASSERT_EQ(2, static_cast<int64_t>(rows.size()));
  EXPECT_TRUE(rows.at(1).at(1).is_null_);
  EXPECT_DOUBLE_EQ(1.5, rows.at(1).at(2).double_);
}

TEST_F(FileScanUtilsTest, parquet_scan)
{
  std::string path = write_file("simple.parquet", base64_decode(SIMPLE_PARQUET_B64));
  ObSEArray<FileOption, 4> options;
  FileScanDesc desc;
  ASSERT_EQ(OB_SUCCESS, build_desc("read_parquet", path, options, desc));
  ASSERT_EQ(1, desc.columns_.count());
  EXPECT_EQ(ObString("c1"), desc.columns_.at(0).name_);
  EXPECT_EQ(FileColumnType::BIGINT, desc.columns_.at(0).type_);
  TestRows rows;
  ASSERT_EQ(OB_SUCCESS, scan_all(desc, rows));
  ASSERT_EQ(7, static_cast<int64_t>(rows.size()));
  int64_t sum = 0;
  for (int64_t i = 0; i < static_cast<int64_t>(rows.size()); ++i) {
    ASSERT_FALSE(rows.at(i).at(0).is_null_);
    sum += rows.at(i).at(0).int_;
  }
  EXPECT_EQ(21, sum);

  // truncated or corrupt files are rejected without crashing
  std::string raw = base64_decode(SIMPLE_PARQUET_B64);
  const int64_t cuts[] = {0, 4, 12, static_cast<int64_t>(raw.size()) / 2,
                          static_cast<int64_t>(raw.size()) - 1};
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(cuts) / sizeof(cuts[0])); ++i) {
    std::string p = write_file("cut.parquet", raw.substr(0, cuts[i]));
    FileScanDesc bad;
    EXPECT_NE(OB_SUCCESS, build_desc("read_parquet", p, options, bad)) << cuts[i];
  }
  for (int64_t i = 4; i + 8 < static_cast<int64_t>(raw.size()); i += 7) {
    std::string mutated = raw;
    mutated[i] = static_cast<char>(mutated[i] ^ 0x5A);
    std::string p = write_file("fuzz.parquet", mutated);
    FileScanDesc fuzz;
    if (OB_SUCCESS == build_desc("read_parquet", p, options, fuzz)) {
      TestRows any_rows;
      (void)scan_all(fuzz, any_rows);  // must fail or succeed cleanly
    }
  }
}

TEST_F(FileScanUtilsTest, snappy_and_rle)
{
  // literal "abc" followed by a copy of offset 3 length 6 -> "abcabcabc"
  const char comp[] = {0x09, 0x08, 'a', 'b', 'c', 0x09, 0x03};
  int64_t len = 0;
  ASSERT_EQ(OB_SUCCESS, snappy_uncompressed_length(comp, sizeof(comp), len));
  ASSERT_EQ(9, len);
  char out[9];
  ASSERT_EQ(OB_SUCCESS, snappy_decompress(comp, sizeof(comp), out, len));
  EXPECT_EQ(0, memcmp(out, "abcabcabc", 9));
  // copy offset beyond produced output must be rejected
  const char bad[] = {0x09, 0x08, 'a', 'b', 'c', 0x09, 0x09};
  EXPECT_NE(OB_SUCCESS, snappy_decompress(bad, sizeof(bad), out, 9));

  // RLE run of 5 x value 3, then a bit-packed group of 8 values 0..7 (width 3)
  const uint8_t rle[] = {0x0A, 0x03, 0x03, 0x88, 0xC6, 0xFA};
  RleBitPackedDecoder decoder;
  ASSERT_EQ(OB_SUCCESS, decoder.init(rle, sizeof(rle), 3));
  uint32_t v = 0;
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(OB_SUCCESS, decoder.next(v));
    EXPECT_EQ(3u, v);
  }
  for (uint32_t i = 0; i < 8; ++i) {
    ASSERT_EQ(OB_SUCCESS, decoder.next(v));
    EXPECT_EQ(i, v);
  }
  EXPECT_NE(OB_SUCCESS, decoder.next(v));
}

TEST_F(FileScanUtilsTest, secure_path)
{
  write_file("in.csv", "a\n1\n");
  ObString real;
  EXPECT_EQ(OB_SUCCESS, resolve_secure_path(str(dir_ + "/in.csv"), str(dir_), allocator_, real));
  EXPECT_EQ(str(dir_ + "/in.csv"), real);
  ASSERT_EQ(0, mkdir((dir_ + "/sub").c_str(), 0755));
  EXPECT_EQ(OB_SUCCESS, resolve_secure_path(str(dir_ + "/./sub/../in.csv"), str(dir_), allocator_, real));
  EXPECT_EQ(str(dir_ + "/in.csv"), real);
  EXPECT_NE(OB_SUCCESS, resolve_secure_path(str(dir_ + "/../in.csv"), str(dir_), allocator_, real));
  EXPECT_NE(OB_SUCCESS, resolve_secure_path(ObString("/etc/passwd"), str(dir_), allocator_, real));
  // symlinks pointing outside the secure directory are refused
  std::string link = dir_ + "/link.csv";
  ASSERT_EQ(0, symlink("/etc/passwd", link.c_str()));
  EXPECT_NE(OB_SUCCESS, resolve_secure_path(str(link), str(dir_), allocator_, real));
  // prefix matches must stop at a directory boundary
  bool allowed = true;
  EXPECT_EQ(OB_SUCCESS, check_path_in_secure_dir(str(dir_ + "x/a.csv"), str(dir_), allowed));
  EXPECT_FALSE(allowed);
  EXPECT_EQ(OB_SUCCESS, check_path_in_secure_dir(str(dir_ + "/a.csv"), str(dir_), allowed));
  EXPECT_TRUE(allowed);
  EXPECT_EQ(OB_SUCCESS, check_path_in_secure_dir(ObString("/any/file"), ObString("/"), allowed));
  EXPECT_TRUE(allowed);
  // directories are not readable as files
  int fd = -1;
  FileFingerprint fp;
  EXPECT_NE(OB_SUCCESS, open_input_file(str(dir_), nullptr, fd, fp));
}

TEST_F(FileScanUtilsTest, copy_csv_writer_round_trip)
{
  ObSEArray<FileOption, 4> options;
  FileOption opt;
  opt.name_ = ObString("header");
  opt.type_ = FileOptionValueType::NONE;
  ASSERT_EQ(OB_SUCCESS, options.push_back(opt));
  opt.name_ = ObString("nullstr");
  opt.type_ = FileOptionValueType::STRING;
  opt.str_ = ObString("\\N");
  ASSERT_EQ(OB_SUCCESS, options.push_back(opt));
  std::string target = dir_ + "/out.csv";
  CopyDesc copy_desc;
  ASSERT_EQ(OB_SUCCESS, build_copy_desc(allocator_, str(target), options, str(dir_), copy_desc));
  ASSERT_EQ(OB_SUCCESS, copy_desc.columns_.push_back(ObString("id")));
  ASSERT_EQ(OB_SUCCESS, copy_desc.columns_.push_back(ObString("note")));
  {
    CopyCsvWriter writer;
    ASSERT_EQ(OB_SUCCESS, writer.open(copy_desc));
    ASSERT_EQ(OB_SUCCESS, writer.begin_row());
    ASSERT_EQ(OB_SUCCESS, writer.append_field("1", 1, false));
    ASSERT_EQ(OB_SUCCESS, writer.append_field("say \"hi\",\nok", 12, false));
    ASSERT_EQ(OB_SUCCESS, writer.end_row());
    ASSERT_EQ(OB_SUCCESS, writer.begin_row());
    ASSERT_EQ(OB_SUCCESS, writer.append_field("2", 1, false));
    ASSERT_EQ(OB_SUCCESS, writer.append_field(nullptr, 0, true));
    ASSERT_EQ(OB_SUCCESS, writer.end_row());
    ASSERT_EQ(OB_SUCCESS, writer.begin_row());
    ASSERT_EQ(OB_SUCCESS, writer.append_field("3", 1, false));
    ASSERT_EQ(OB_SUCCESS, writer.append_field("", 0, false));
    ASSERT_EQ(OB_SUCCESS, writer.end_row());
    ASSERT_EQ(OB_SUCCESS, writer.commit());
  }
  EXPECT_EQ("id,note\n1,\"say \"\"hi\"\",\nok\"\n2,\\N\n3,\"\"\n", read_file(target));

  // the written file is readable by read_csv with the same null string
  ObSEArray<FileOption, 4> read_options;
  opt.name_ = ObString("nullstr");
  ASSERT_EQ(OB_SUCCESS, read_options.push_back(opt));
  FileScanDesc desc;
  ASSERT_EQ(OB_SUCCESS, build_desc("read_csv", target, read_options, desc));
  TestRows rows;
  ASSERT_EQ(OB_SUCCESS, scan_all(desc, rows));
  ASSERT_EQ(3, static_cast<int64_t>(rows.size()));
  EXPECT_EQ(ObString("say \"hi\",\nok"), rows.at(0).at(1).str());
  EXPECT_TRUE(rows.at(1).at(1).is_null_);
  EXPECT_FALSE(rows.at(2).at(1).is_null_);

  // an existing target is never overwritten
  CopyDesc again;
  EXPECT_NE(OB_SUCCESS, build_copy_desc(allocator_, str(target), options, str(dir_), again));
  // an aborted copy leaves no file behind
  std::string target2 = dir_ + "/aborted.csv";
  CopyDesc desc2;
  ASSERT_EQ(OB_SUCCESS, build_copy_desc(allocator_, str(target2), options, str(dir_), desc2));
  {
    CopyCsvWriter writer;
    ASSERT_EQ(OB_SUCCESS, writer.open(desc2));
    ASSERT_EQ(OB_SUCCESS, writer.begin_row());
    ASSERT_EQ(OB_SUCCESS, writer.append_field("1", 1, false));
    ASSERT_EQ(OB_SUCCESS, writer.end_row());
    writer.abort();
  }
  struct stat st;
  EXPECT_NE(0, stat(target2.c_str(), &st));
  std::string cmd = "test -z \"$(ls -A '" + dir_ + "' | grep '\\.tmp$')\"";
  EXPECT_EQ(0, system(cmd.c_str()));
  // targets outside secure_file_priv are refused
  CopyDesc outside;
  EXPECT_NE(OB_SUCCESS, build_copy_desc(allocator_, ObString("/etc/out.csv"), options, str(dir_),
                                        outside));
}

}  // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
