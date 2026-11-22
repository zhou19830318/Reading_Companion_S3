#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "ReaderQueryCodec.h"
#include "SyncCodec.h"

namespace {

TEST(SyncPush, BuildsBatchBody) {
  const char* ops[] = {R"({"op_id":"a","type":"ai_result.create"})", R"({"op_id":"b","type":"ai_result.create"})"};
  char out[512];
  const size_t len = Sync::buildPushRequest(out, sizeof(out), "onepage-s3", "batch1", ops, 2);
  ASSERT_GT(len, 0u);
  const std::string body(out);
  EXPECT_NE(std::string::npos, body.find("\"device_id\":\"onepage-s3\""));
  EXPECT_NE(std::string::npos, body.find("\"batch_id\":\"batch1\""));
  const std::string expectedOps =
      "\"operations\":[{\"op_id\":\"a\",\"type\":\"ai_result.create\"},"
      "{\"op_id\":\"b\",\"type\":\"ai_result.create\"}]}";
  EXPECT_NE(std::string::npos, body.find(expectedOps));
}

TEST(SyncPush, RejectsEmptyOrOversizedInput) {
  char out[32];
  const char* ops[] = {R"({"op_id":"a"})"};
  EXPECT_EQ(0u, Sync::buildPushRequest(out, sizeof(out), "d", "b", nullptr, 0));
  EXPECT_EQ(0u, Sync::buildPushRequest(out, 4, "d", "b", ops, 1));
}

TEST(SyncPushResponse, MarksAllAckedOnlyWhenEverythingApplied) {
  const char* json = R"({"batch_id":"b","results":[
    {"op_id":"a","status":"applied","server_id":"x","error":null},
    {"op_id":"b","status":"duplicate","server_id":"y","error":null}]})";
  Sync::PushResult results[4];
  bool allAcked = false;
  const size_t n = Sync::parsePushResponse(json, std::strlen(json), results, 4, 2, allAcked);
  ASSERT_EQ(2u, n);
  EXPECT_TRUE(allAcked);
  EXPECT_STREQ("a", results[0].opId);
  EXPECT_EQ(Sync::PushStatus::Applied, results[0].status);
  EXPECT_EQ(Sync::PushStatus::Duplicate, results[1].status);
}

TEST(SyncPushResponse, ConflictKeepsOutbox) {
  const char* json = R"({"results":[{"op_id":"a","status":"rejected","error":"bad"},
                                          {"op_id":"b","status":"conflict","error":"changed"}]})";
  Sync::PushResult results[4];
  bool allAcked = true;
  ASSERT_EQ(2u, Sync::parsePushResponse(json, std::strlen(json), results, 4, 2, allAcked));
  EXPECT_FALSE(allAcked);
  EXPECT_EQ(Sync::PushStatus::Rejected, results[0].status);
  EXPECT_EQ(Sync::PushStatus::Conflict, results[1].status);
}

TEST(SyncPushResponse, MissingResultIsNotAllAcked) {
  const char* json = R"({"results":[{"op_id":"a","status":"applied"}]})";
  Sync::PushResult results[4];
  bool allAcked = true;
  ASSERT_EQ(1u, Sync::parsePushResponse(json, std::strlen(json), results, 4, 2, allAcked));
  EXPECT_FALSE(allAcked);
}

TEST(SyncAiResult, SerializesSegmentsAndCitations) {
  ReaderQuery::Response response;
  const std::string text = "最简单直接的智能指针是 box \"quoted\"";
  std::memcpy(response.text, text.data(), text.size());
  response.textLen = static_cast<uint16_t>(text.size());
  response.segmentCount = 1;
  response.segments[0].textOffset = 0;
  response.segments[0].textLen = static_cast<uint16_t>(text.size());
  response.segments[0].source = ReaderQuery::Source::Book;
  response.segments[0].firstCitation = 0;
  response.segments[0].citationCount = 1;
  response.citationCount = 1;
  std::strcpy(response.citations[0].passageId, "808df033d70dfa26:67:p0");
  std::strcpy(response.citations[0].quote, "最简单直接的智能指针是 box");
  response.citations[0].spineIndex = 67;

  const char* ids[] = {"808df033d70dfa26:67:p0"};
  char out[2048];
  const size_t len = ReaderQuery::buildAiResultOp(out, sizeof(out), "op123", "ai_123", "808df033d70dfa26", ids, 1,
                                                  "这一章讲了什么？", 1763550000000ULL, response);
  ASSERT_GT(len, 0u);
  const std::string op(out);
  EXPECT_NE(std::string::npos, op.find("\"type\":\"ai_result.create\""));
  EXPECT_NE(std::string::npos, op.find("\"client_ts\":1763550000000"));
  EXPECT_NE(std::string::npos, op.find("\"passage_ids\":[\"808df033d70dfa26:67:p0\"]"));
  EXPECT_NE(std::string::npos, op.find("\"source\":\"book\""));
  EXPECT_NE(std::string::npos, op.find("\\\"quoted\\\""));
  EXPECT_NE(std::string::npos, op.find("\"spine_index\":67"));
  EXPECT_NE(std::string::npos, op.find("\"quote\":\"最简单直接的智能指针是 box\""));
}

TEST(SyncNote, SerializesNoteCreateWithAnchorAndEscapes) {
  char out[1024];
  const size_t len =
      Sync::buildNoteCreateOp(out, sizeof(out), "op123", "nt_123", "808df033d70dfa26", "", "山间的书店有一盏灯。",
                              "这是一条\"语音\"笔记", "note", 3, 7, 12, 1763550000000ULL);
  ASSERT_GT(len, 0u);
  const std::string op(out);
  EXPECT_NE(std::string::npos, op.find("\"type\":\"note.create\""));
  EXPECT_NE(std::string::npos, op.find("\"client_ts\":1763550000000"));
  EXPECT_NE(std::string::npos, op.find("\"book_id\":\"808df033d70dfa26\""));
  EXPECT_NE(std::string::npos, op.find("\"quote\":\"山间的书店有一盏灯。\""));
  EXPECT_NE(std::string::npos, op.find("\\\"语音\\\""));
  EXPECT_NE(std::string::npos, op.find("\"spine_index\":3"));
  EXPECT_NE(std::string::npos, op.find("\"page_number\":7"));
  EXPECT_NE(std::string::npos, op.find("\"page_count\":12"));
}

TEST(SyncNote, ReplacesInvalidOrTruncatedUtf8InJson) {
  // A byte-budget cut can leave the last UTF-8 code point incomplete, and the
  // JSON body must stay valid UTF-8 for the relay to parse it.
  const char quote[] =
      "\xE5\xB1\xB1\xE9\x97\xB4\xE4\xB9\xA6\xE5\xBA\x97\xE6\x9C\x89\xE4\xB8\x80\xE7\x9B\x8F\xE7\x81\xAF\xE3\x80\x82\xE5"
      "\xBD\x93\xEF\xBF";
  char out[1024];
  const size_t len =
      Sync::buildNoteCreateOp(out, sizeof(out), "op123", "nt_123", "bookid", "", quote, "note", "note", 0, 1, 1, 1);
  ASSERT_GT(len, 0u);
  const std::string op(out, len);
  EXPECT_EQ(std::string::npos, op.find("\xEF\xBF\""));
  EXPECT_NE(std::string::npos, op.find("\xEF\xBF\xBD"));
  // The valid prefix is preserved byte-for-byte.
  EXPECT_NE(std::string::npos, op.find("\xE5\xB1\xB1\xE9\x97\xB4\xE4\xB9\xA6\xE5\xBA\x97"));
}

TEST(SyncNote, RejectsMissingBookIdAndShortBuffer) {
  char out[512];
  EXPECT_EQ(0u, Sync::buildNoteCreateOp(out, sizeof(out), "op", "nt", "", "", "", "note", "note", 0, 0, 0, 1));
  EXPECT_EQ(0u, Sync::buildNoteCreateOp(out, 16, "op", "nt", "book", "", "", "note", "note", 0, 0, 0, 1));
}

}  // namespace
