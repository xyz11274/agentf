/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "model/ModelUtils.h"
#include "aisf_platform/parser/json_parser.h"
#include <regex>
#include <vector>

class TestParseBertParamTag: public ::testing::Test
{
protected:
   static void SetUpTestCase()
   {
   }

   static void TearDownTestCase()
   {
   }

   virtual void SetUp()
   {
   }

   virtual void TearDown()
   {
       GlobalMockObject::verify();
   }
};

TEST_F(TestParseBertParamTag, EmptyInput) {
    // Given
    std::string input = "";

    // When
    std::vector<BertParamTag> result = parseBertParamTag(input);

    // Then
    EXPECT_EQ(result.size(), 0);

}

TEST_F(TestParseBertParamTag, NoTags) {
    // Given
    std::string input = "This is a test string without tags";

    // When
    std::vector<BertParamTag> result = parseBertParamTag(input);

    // Then
    EXPECT_EQ(result.size(), 0);

}

TEST_F(TestParseBertParamTag, SingleTagNoColon) {
    // Given
    std::string input = "[tagWithoutColon]";

    // When
    std::vector<BertParamTag> result = parseBertParamTag(input);

    // Then
    EXPECT_EQ(result.size(), 0);

}

TEST_F(TestParseBertParamTag, SingleTagNoHyphens) {
    // Given
    std::string input = "[tag:textOnly:]";

    // When
    std::vector<BertParamTag> result = parseBertParamTag(input);

    // Then
    EXPECT_EQ(result.size(), 0);

}

TEST_F(TestParseBertParamTag, SingleValidTag) {
    // Given
    std::string input = "[tag:type-name:]";

    // When
    std::vector<BertParamTag> result = parseBertParamTag(input);

    // Then
    EXPECT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].text, "tag");
    EXPECT_EQ(result[0].type, "type");
    EXPECT_EQ(result[0].name, "name:");

}

TEST_F(TestParseBertParamTag, MultipleTags) {
    // Given
    std::string input = "[tag1:type1-name1:][tag2:type2-name2:]";

    // When
    std::vector<BertParamTag> result = parseBertParamTag(input);

    // Then
    EXPECT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].text, "tag1");
    EXPECT_EQ(result[0].type, "type1");
    EXPECT_EQ(result[0].name, "name1:");
    EXPECT_EQ(result[1].text, "tag2");
    EXPECT_EQ(result[1].type, "type2");
    EXPECT_EQ(result[1].name, "name2:");

}

TEST_F(TestParseBertParamTag, MixedContent) {
    // Given
    std::string input = "Some text [tag:type-name:] more text [anotherTag:otherType-otherName:]";

    // When
    std::vector<BertParamTag> result = parseBertParamTag(input);

    // Then
    EXPECT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].text, "tag");
    EXPECT_EQ(result[0].type, "type");
    EXPECT_EQ(result[0].name, "name:");
    EXPECT_EQ(result[1].text, "anotherTag");
    EXPECT_EQ(result[1].type, "otherType");
    EXPECT_EQ(result[1].name, "otherName:");

}

