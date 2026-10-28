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
// AI_PROMPT template rendering: JSON args, repeated / nested / illegal placeholders, Unicode.

#include <gtest/gtest.h>
#include "ob_ai_func_test_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::unittest;

class ObAIPromptTest : public ::testing::Test
{
protected:
  ObJsonObject *prompt(const char *text)
  {
    ObJsonObject *obj = parse_json_object(allocator_, text);
    EXPECT_NE(nullptr, obj);
    return obj;
  }
  // render a prompt object, returns the error code and the rendered text
  int render(const char *text, std::string &out)
  {
    ObString rendered;
    int ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt(text), rendered);
    out = to_std_string(rendered);
    return ret;
  }
  std::string render_ok(const char *text)
  {
    std::string out;
    EXPECT_EQ(OB_SUCCESS, render(text, out)) << text;
    return out;
  }
  ObArenaAllocator allocator_;
};

TEST_F(ObAIPromptTest, test_ob_is_vaild_prompt_object)
{
  ObString a("a");
  ObJsonString a_str(a);
  ObJsonObject *obj = prompt("{\"template\": \"{0}+{1}={2} 吗？请回答true或false\", \"args\": [\"1\", \"2\", \"3\"]}");
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(obj));
  obj->add("a", &a_str);
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(obj));
}

TEST_F(ObAIPromptTest, test_is_valid_prompt_object_shape)
{
  EXPECT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(nullptr));
  EXPECT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt(R"J({"template":1,"args":[]})J")));
  EXPECT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt(R"J({"template":"t","args":"x"})J")));
  EXPECT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt(R"J({"template":"t","arg":[]})J")));
  // arguments may be any json value
  EXPECT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(
      prompt(R"J({"template":"{0}{1}{2}","args":[[1,true],{"k":2},null]})J")));
}

TEST_F(ObAIPromptTest, test_replace_all_str_args_in_template)
{
  EXPECT_EQ("1+2=3 吗？请回答true或false",
            render_ok("{\"template\": \"{0}+{1}={2} 吗？请回答true或false\", \"args\": [\"1\", \"2\", \"3\"]}"));
}

TEST_F(ObAIPromptTest, test_repeated_placeholder)
{
  // every occurrence is replaced, the result may be much longer than template + args
  EXPECT_EQ("abcdefghij/abcdefghij/abcdefghij/abcdefghij-x",
            render_ok(R"J({"template":"{0}/{0}/{0}/{0}-{1}","args":["abcdefghij","x"]})J"));
  EXPECT_EQ("b a b", render_ok(R"J({"template":"{1} {0} {1}","args":["a","b"]})J"));
  EXPECT_EQ("unused args are fine", render_ok(R"J({"template":"unused args are fine","args":["a"]})J"));
}

TEST_F(ObAIPromptTest, test_json_array_and_object_args)
{
  EXPECT_EQ("[1, true]/[1, true]/{\"k\": 2}",
            render_ok(R"J({"template":"{0}/{0}/{1}","args":[[1,true],{"k":2}]})J"));
  EXPECT_EQ("n=null b=false d=1.5", render_ok(R"J({"template":"n={0} b={1} d={2}","args":[null,false,1.5]})J"));
  // strings inside JSON args keep their JSON escaping
  EXPECT_EQ("[\"a\\\"b\"]", render_ok(R"J({"template":"{0}","args":[["a\"b"]]})J"));
}

TEST_F(ObAIPromptTest, test_nested_prompt)
{
  EXPECT_EQ("outer inner xx / y",
            render_ok(R"J({"template":"outer {0} / {1}","args":[{"template":"inner {0}{0}","args":["x"]},"y"]})J"));
  // two levels of nesting
  EXPECT_EQ("L0(L1(L2))",
            render_ok(R"J({"template":"L0({0})","args":[{"template":"L1({0})","args":[{"template":"L2","args":[]}]}]})J"));
  // an object that is not a prompt is printed as json
  EXPECT_EQ("{\"template\": \"t\"}", render_ok(R"J({"template":"{0}","args":[{"template":"t"}]})J"));
  // an illegal placeholder inside a nested prompt fails the whole prompt
  std::string out;
  EXPECT_EQ(OB_INVALID_ARGUMENT,
            render(R"J({"template":"{0}","args":[{"template":"{5}","args":["x"]}]})J", out));
}

TEST_F(ObAIPromptTest, test_nested_prompt_depth_limit)
{
  std::string text = "\"leaf\"";
  // deeper than the nested prompt limit, still within the json parser depth limit
  for (int i = 0; i < 20; i++) {
    text = "{\"template\":\"{0}\",\"args\":[" + text + "]}";
  }
  std::string out;
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(text.c_str(), out));
}

TEST_F(ObAIPromptTest, test_illegal_placeholder_index)
{
  std::string out;
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"J({"template":"{1}","args":["x"]})J", out));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"J({"template":"{0}-{3}","args":["a"]})J", out));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"J({"template":"{0}","args":[]})J", out));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"J({"template":"{999999999999999999999}","args":["x"]})J", out));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"J({"template":"","args":["x"]})J", out));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"J({"template":"{0}","args":"x"})J", out));
}

TEST_F(ObAIPromptTest, test_non_placeholder_braces_are_literal)
{
  // only '{' digits '}' is a placeholder, everything else is kept as is
  EXPECT_EQ("{} {a} {0 } {-1} { 0} {x",
            render_ok(R"J({"template":"{} {a} {0 } {-1} { 0} {x","args":["v"]})J"));
  EXPECT_EQ("{v}", render_ok(R"J({"template":"{{0}}","args":["v"]})J"));
  EXPECT_EQ("v}", render_ok(R"J({"template":"{0}}","args":["v"]})J"));
  EXPECT_EQ("tail {", render_ok(R"J({"template":"tail {","args":["v"]})J"));
  EXPECT_EQ("v", render_ok(R"J({"template":"{00}","args":["v"]})J"));
}

TEST_F(ObAIPromptTest, test_unicode_and_escape)
{
  EXPECT_EQ("你好世界，emoji😀ü", render_ok(R"J({"template":"你好{0}，emoji😀{1}","args":["世界","ü"]})J"));
  // json escapes in template and args are decoded before rendering
  EXPECT_EQ("quote \"a\"b\" back\\slash c\\d\nnew line \u00e9",
            render_ok(R"J({"template":"quote \"{0}\" back\\slash {1}\nnew line \u00e9","args":["a\"b","c\\d"]})J"));
  // placeholders inside arguments are not expanded again
  EXPECT_EQ("{1}-x", render_ok(R"J({"template":"{0}-{1}","args":["{1}","x"]})J"));
  EXPECT_EQ("", render_ok(R"J({"template":"{0}","args":[""]})J"));
}
