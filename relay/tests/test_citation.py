"""Citation parsing: fences, non-JSON, forged quotes and source normalization."""

from __future__ import annotations

import json

from relay.citation import parse_model_output, validate_segments

PID = "9e08598fd2076fdb:0:p0"
PASSAGE = {
    "passage_id": PID,
    "spine_index": 0,
    "kind": "p",
    "block_index": 0,
    "text": "山间的书店有一盏灯。",
}
ALLOWED = {PID: PASSAGE}
OTHER_SPINE = {
    "passage_id": "9e08598fd2076fdb:1:p0",
    "spine_index": 1,
    "kind": "p",
    "block_index": 0,
    "text": "傍晚的时候，灯变得更亮了。",
}


def _segments(payload):
    return validate_segments(payload, allowed=ALLOWED)


def test_valid_book_citation_kept():
    segments, warnings = _segments(
        [
            {
                "source": "book",
                "text": "开头一句。",
                "citations": [{"passage_id": PID, "quote": "山间的书店有一盏灯。"}],
            }
        ]
    )
    assert warnings == []
    assert segments == [
        {
            "source": "book",
            "text": "开头一句。",
            "citations": [
                {"passage_id": PID, "spine_index": 0, "quote": "山间的书店有一盏灯。"}
            ],
        }
    ]


def test_forged_quote_drops_citation_and_downgrades():
    segments, warnings = _segments(
        [{"source": "book", "text": "伪造。", "citations": [{"passage_id": PID, "quote": "书里没有这句"}]}]
    )
    assert segments[0]["source"] == "inference"
    assert segments[0]["citations"] == []
    assert "CITATION_DROPPED" in warnings


def test_citation_to_passage_outside_context_dropped():
    segments, warnings = _segments(
        [{"source": "book", "text": "越界。", "citations": [{"passage_id": OTHER_SPINE["passage_id"], "quote": "傍晚的时候，灯变得更亮了。"}]}]
    )
    assert segments[0]["source"] == "inference"
    assert "CITATION_DROPPED" in warnings


def test_book_without_citations_downgrades():
    segments, warnings = _segments([{"source": "book", "text": "没有引用。"}])
    assert segments[0]["source"] == "inference"
    assert "CITATION_DROPPED" in warnings


def test_external_citations_are_not_book_grounded():
    segments, warnings = _segments(
        [
            {
                "source": "external",
                "text": "作者生平。",
                "citations": [{"passage_id": PID, "quote": "山间的书店有一盏灯。"}],
            }
        ]
    )
    assert segments[0]["source"] == "external"
    assert segments[0]["citations"] == []
    assert warnings == []


def test_source_aliases_and_unknown_source():
    segments, _ = _segments(
        [
            {"source": "书中原文", "text": "一", "citations": [{"passage_id": PID, "quote": "山间的书店有一盏灯。"}]},
            {"source": "外部补充", "text": "二"},
            {"source": "what", "text": "三"},
        ]
    )
    assert [s["source"] for s in segments] == ["book", "external", "inference"]


def test_chapter_scope_rejects_other_spine():
    segments, warnings = validate_segments(
        [
            {
                "source": "book",
                "text": "跨章引用。",
                "citations": [{"passage_id": OTHER_SPINE["passage_id"], "quote": "傍晚的时候，灯变得更亮了。"}],
            }
        ],
        allowed={PID: PASSAGE, OTHER_SPINE["passage_id"]: OTHER_SPINE},
        current_spine=0,
        chapter_scope=True,
    )
    assert segments[0]["source"] == "inference"
    assert "CITATION_DROPPED" in warnings


def test_whitespace_in_quote_normalizes_before_match():
    segments, warnings = _segments(
        [{"source": "book", "text": "空白。", "citations": [{"passage_id": PID, "quote": "  山间的书店  有一盏灯。 "}]}]
    )
    assert warnings == []
    assert segments[0]["citations"][0]["quote"] == "山间的书店有一盏灯。"


def test_fenced_json_is_parsed():
    text = "```json\n" + json.dumps(
        {
            "segments": [
                {
                    "source": "book",
                    "text": "开头。",
                    "citations": [{"passage_id": PID, "quote": "山间的书店有一盏灯。"}],
                }
            ]
        },
        ensure_ascii=False,
    ) + "\n```"
    segments, warnings = parse_model_output(text, allowed=ALLOWED)
    assert warnings == []
    assert segments[0]["source"] == "book"


def test_json_with_prose_preamble_is_parsed():
    text = "好的，下面是回答：\n" + json.dumps(
        {"segments": [{"source": "inference", "text": "推断。"}]}, ensure_ascii=False
    ) + "\n希望有帮助。"
    segments, warnings = parse_model_output(text, allowed=ALLOWED)
    assert warnings == []
    assert segments[0]["source"] == "inference"


def test_non_json_falls_back_to_single_inference_segment():
    segments, warnings = parse_model_output("这是一段自然语言回答。", allowed=ALLOWED)
    assert segments == [{"source": "inference", "text": "这是一段自然语言回答。", "citations": []}]
    assert warnings == ["MODEL_OUTPUT_NOT_JSON"]


def test_empty_model_output_yields_no_segments():
    segments, warnings = parse_model_output("   ", allowed=ALLOWED)
    assert segments == []
    assert warnings == ["MODEL_OUTPUT_NOT_JSON"]


def test_answer_shorthand_object():
    segments, warnings = parse_model_output('{"answer": "直接回答"}', allowed=ALLOWED)
    assert warnings == []
    assert segments == [{"source": "inference", "text": "直接回答", "citations": []}]


def test_empty_segment_text_is_dropped():
    segments, _ = _segments([{"source": "inference", "text": "   "}, {"source": "inference", "text": "有效"}])
    assert [s["text"] for s in segments] == ["有效"]


def test_citation_cap_per_segment():
    citations = [{"passage_id": PID, "quote": "山间的书店有一盏灯。"}] * 12
    segments, _ = _segments([{"source": "book", "text": "多引用。", "citations": citations}])
    assert len(segments[0]["citations"]) == 8
