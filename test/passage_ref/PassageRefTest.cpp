#include <gtest/gtest.h>

#include <vector>

#include "Epub/PassageRef.h"

namespace {

using EpubPassage::Kind;

TEST(PassageRef, FormatsParagraphAndListItemIds) {
  char out[40];
  ASSERT_EQ(EpubPassage::formatId(out, sizeof(out), "a1b2c3d4e5f60718", 3, {Kind::Paragraph, 0}), 21u);
  EXPECT_STREQ(out, "a1b2c3d4e5f60718:3:p0");

  ASSERT_GT(EpubPassage::formatId(out, sizeof(out), "a1b2c3d4e5f60718", 12, {Kind::ListItem, 17}), 0u);
  EXPECT_STREQ(out, "a1b2c3d4e5f60718:12:li17");
}

TEST(PassageRef, RejectsInvalidOrUndersizedOutput) {
  // The point of the test is exactly that a too-small buffer returns 0, so
  // the deliberate truncation warning is disabled for this target in CMake.
  std::vector<char> out(8, '\0');
  EXPECT_EQ(EpubPassage::formatId(out.data(), out.size(), "a1b2c3d4e5f60718", 0, {Kind::Paragraph, 0}), 0u);
  EXPECT_EQ(EpubPassage::formatId(out.data(), out.size(), "a1b2c3d4e5f60718", 0, {}), 0u);
  EXPECT_EQ(EpubPassage::formatId(nullptr, 8, "a1b2c3d4e5f60718", 0, {Kind::Paragraph, 0}), 0u);
}

TEST(PassageRef, KindPrefixMatchesContract) {
  EXPECT_STREQ(EpubPassage::kindPrefix(Kind::Paragraph), "p");
  EXPECT_STREQ(EpubPassage::kindPrefix(Kind::ListItem), "li");
  EXPECT_STREQ(EpubPassage::kindPrefix(Kind::None), "");
}

}  // namespace
