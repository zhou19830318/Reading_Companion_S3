#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

#include "ReaderQueryCodec.h"

namespace {

using ReaderQuery::MAX_PASSAGE_IDS;
using ReaderQuery::Response;
using ReaderQuery::Source;
using ReaderQuery::WARN_CITATION_DROPPED;
using ReaderQuery::WARN_MODEL_OUTPUT_NOT_JSON;
using ReaderQuery::WARN_TRUNCATED;

std::unique_ptr<Response> newResponse() { return std::make_unique<Response>(); }

TEST(ReaderQueryRequest, BuildsContractBody) {
  const char* ids[] = {"a1b2c3d4e5f60718:1:p0", "a1b2c3d4e5f60718:1:li3"};
  ReaderQuery::Request request;
  request.requestId = "onepage-s3:00001234";
  request.bookId = "a1b2c3d4e5f60718";
  request.spineIndex = 1;
  request.passageIds = ids;
  request.passageIdCount = 2;
  request.selectedText = "quote \"with\" slash\\";
  request.question = "这句话起什么作用？";
  request.intent = "explain";
  request.contextScope = "selection";
  request.language = "zh";

  char out[1024];
  const size_t len = ReaderQuery::buildRequest(out, sizeof(out), request);
  ASSERT_GT(len, 0u);
  EXPECT_EQ(len, std::strlen(out));

  EXPECT_NE(std::string::npos, std::string(out).find("\"request_id\":\"onepage-s3:00001234\""));
  EXPECT_NE(std::string::npos, std::string(out).find("\"book_id\":\"a1b2c3d4e5f60718\""));
  EXPECT_NE(std::string::npos, std::string(out).find("\"spine_index\":1"));
  EXPECT_NE(std::string::npos, std::string(out).find("\"passage_ids\":[\"a1b2c3d4e5f60718:1:p0\","
                                                     "\"a1b2c3d4e5f60718:1:li3\"]"));
  EXPECT_NE(std::string::npos, std::string(out).find("\"selected_text\":\"quote \\\"with\\\" slash\\\\\""));
  EXPECT_NE(std::string::npos, std::string(out).find("\"context_scope\":\"selection\""));
  EXPECT_NE(std::string::npos, std::string(out).find("\"max_output_tokens\":512"));
}

TEST(ReaderQueryRequest, CapsPassageIds) {
  const char* ids[MAX_PASSAGE_IDS + 1] = {};
  for (size_t i = 0; i <= MAX_PASSAGE_IDS; ++i) ids[i] = "a1b2c3d4e5f60718:0:p0";
  ReaderQuery::Request request;
  request.requestId = "r";
  request.bookId = "a1b2c3d4e5f60718";
  request.passageIds = ids;
  request.passageIdCount = MAX_PASSAGE_IDS + 1;

  char out[1024];
  ASSERT_GT(ReaderQuery::buildRequest(out, sizeof(out), request), 0u);
  const std::string body(out);
  size_t count = 0;
  for (size_t pos = body.find("a1b2c3d4e5f60718:0:p0"); pos != std::string::npos;
       pos = body.find("a1b2c3d4e5f60718:0:p0", pos + 1)) {
    ++count;
  }
  EXPECT_EQ(count, MAX_PASSAGE_IDS);
}

TEST(ReaderQueryRequest, RejectsBadArgumentsAndTinyBuffer) {
  char out[16];
  ReaderQuery::Request request;
  request.requestId = "r";
  request.bookId = "b";
  EXPECT_EQ(0u, ReaderQuery::buildRequest(out, sizeof(out), request));

  request.requestId = nullptr;
  EXPECT_EQ(0u, ReaderQuery::buildRequest(out, sizeof(out), request));
}

TEST(ReaderQueryResponse, ParsesSegmentsAndCitations) {
  const char* json = R"({
    "request_id": "onepage-s3:00001234",
    "book_id": "a1b2c3d4e5f60718",
    "segments": [
      {"source": "book", "text": "作者先用一盏灯把空间点亮。",
       "citations": [{"passage_id": "a1b2c3d4e5f60718:1:p0", "spine_index": 1,
                      "quote": "山间的书店有一盏灯。"}]},
      {"source": "inference", "text": "这也暗示了书店是一个避风港。", "citations": []},
      {"source": "external", "text": "外部补充。", "citations": []}
    ],
    "warnings": [],
    "usage": {"input_tokens": 1200, "output_tokens": 180},
    "model": "qwen-flash"
  })";

  auto response = newResponse();
  ASSERT_TRUE(ReaderQuery::parseResponse(json, std::strlen(json), "onepage-s3:00001234", *response));
  EXPECT_STREQ(response->requestId, "onepage-s3:00001234");
  EXPECT_STREQ(response->bookId, "a1b2c3d4e5f60718");
  ASSERT_EQ(3, response->segmentCount);
  EXPECT_EQ(Source::Book, response->segments[0].source);
  EXPECT_EQ(Source::Inference, response->segments[1].source);
  EXPECT_EQ(Source::External, response->segments[2].source);

  ASSERT_EQ(1, response->citationCount);
  EXPECT_STREQ(response->citations[0].passageId, "a1b2c3d4e5f60718:1:p0");
  EXPECT_STREQ(response->citations[0].quote, "山间的书店有一盏灯。");
  EXPECT_EQ(1, response->citations[0].spineIndex);

  const auto& first = response->segments[0];
  EXPECT_EQ(1, first.citationCount);
  EXPECT_EQ(0, first.firstCitation);
  EXPECT_EQ(std::string("作者先用一盏灯把空间点亮。"), std::string(response->text + first.textOffset, first.textLen));
  const auto& second = response->segments[1];
  EXPECT_EQ(std::string("这也暗示了书店是一个避风港。"),
            std::string(response->text + second.textOffset, second.textLen));
}

TEST(ReaderQueryResponse, DropsCitationsWithoutEvidenceAndFlagsWarnings) {
  const char* json = R"({
    "request_id": "r1",
    "book_id": "b1",
    "segments": [
      {"source": "book", "text": "无引用的书中原文。",
       "citations": [{"passage_id": "x", "quote": ""}]}
    ],
    "warnings": ["CITATION_DROPPED", "MODEL_OUTPUT_NOT_JSON"]
  })";
  auto response = newResponse();
  ASSERT_TRUE(ReaderQuery::parseResponse(json, std::strlen(json), nullptr, *response));
  ASSERT_EQ(1, response->segmentCount);
  EXPECT_EQ(0, response->segments[0].citationCount);
  EXPECT_EQ(0, response->citationCount);
  EXPECT_NE(0u, response->warnings & WARN_CITATION_DROPPED);
  EXPECT_NE(0u, response->warnings & WARN_MODEL_OUTPUT_NOT_JSON);
}

TEST(ReaderQueryResponse, LongTextUsesChunkedDelivery) {
  const std::string longText(2000, 'x');
  const std::string json =
      "{\"request_id\":\"r\",\"book_id\":\"b\",\"segments\":[{\"source\":\"inference\","
      "\"text\":\"" +
      longText + "\"}]}";
  auto response = newResponse();
  ASSERT_TRUE(ReaderQuery::parseResponse(json.c_str(), json.size(), nullptr, *response));
  ASSERT_EQ(1, response->segmentCount);
  EXPECT_EQ(2000u, response->segments[0].textLen);
  EXPECT_EQ(longText, std::string(response->text + response->segments[0].textOffset, response->segments[0].textLen));
}

TEST(ReaderQueryResponse, RejectsStaleRequestIdAndEmptyBody) {
  const char* json = R"({"request_id":"old","book_id":"b",
    "segments":[{"source":"inference","text":"x"}]})";
  auto response = newResponse();
  EXPECT_FALSE(ReaderQuery::parseResponse(json, std::strlen(json), "new", *response));

  EXPECT_FALSE(ReaderQuery::parseResponse("", 0, nullptr, *response));
  EXPECT_FALSE(ReaderQuery::parseResponse("not json", 8, nullptr, *response));
}

TEST(ReaderQueryResponse, UnknownSourceFallsBackToInference) {
  const char* json = R"({"request_id":"r","book_id":"b",
    "segments":[{"source":"weird","text":"x"}]})";
  auto response = newResponse();
  ASSERT_TRUE(ReaderQuery::parseResponse(json, std::strlen(json), nullptr, *response));
  EXPECT_EQ(Source::Inference, response->segments[0].source);
}

}  // namespace
