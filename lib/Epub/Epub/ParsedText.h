#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;      // true = word attaches to previous with no break
  std::vector<bool> wordNoSpaceBefore;  // true = may break before token, but no synthetic space when joined
  std::vector<bool> wordIsFocusSuffix;  // true = token is the regular tail of a focus bold-prefix split
  // Passage mapping (docs/ai-native/api-v1.md). Each token records its UTF-8
  // byte range inside the *normalized* passage text, so a laid-out line can
  // carry a citation range. Empty/zero-length tokens (the <li> bullet) have
  // start == end and are ignored when a line's range is computed.
  std::vector<uint32_t> wordPassageStart;
  std::vector<uint32_t> wordPassageEnd;
  EpubPassage::Ref passage;
  // Byte offset in the passage text where the next word starts. Continues
  // across <br>-created text blocks of the same passage so line ranges stay
  // monotonic.
  uint32_t passageCursor = 0;
  uint32_t passageLastCp = 0;
  bool passageHasText = false;
  // Set by a layout-only &nbsp; space token; committed (or dropped by the CJK
  // gap rules) when the following word arrives.
  bool passagePendingSpace = false;
  // Set after an inline image that sits directly between two words: the relay
  // contributes no bytes for <img>, so the next word must not get a synthetic
  // space in the citation range.
  bool passageSuppressNextGap = false;
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool isNaturalAlign;
  bool hasRtlWord;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<bool> reorderedFocusSuffixScratch;
  std::vector<uint16_t> visualOrderScratch;

  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const bool focusReadingEnabled = false, const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               bool countsForPassage = true);
  // Binds this paragraph to a source <p>/<li>. cursor/hasText/lastCp carry the
  // passage position across a layout block break (e.g. <br>) so the next word
  // continues the same byte range.
  void setPassage(const EpubPassage::Ref ref, const uint32_t cursor = 0, const bool hasText = false,
                  const uint32_t lastCp = 0) {
    passage = ref;
    passageCursor = cursor;
    passageHasText = hasText;
    passageLastCp = lastCp;
    passagePendingSpace = false;
    passageSuppressNextGap = false;
  }
  void clearPassage() { setPassage({}); }
  void setSuppressNextPassageGap(const bool suppress) { passageSuppressNextGap = suppress; }
  const EpubPassage::Ref& getPassage() const { return passage; }
  uint32_t getPassageCursor() const { return passageCursor; }
  uint32_t getPassageLastCp() const { return passageLastCp; }
  bool getPassageHasText() const { return passageHasText; }
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};
