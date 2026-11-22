"""Golden books must produce the exact passage IDs defined by api-v1.md."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from relay.book_ingest import normalize_text, parse_book

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "golden_books"
MANIFEST = json.loads((FIXTURES / "golden_manifest.json").read_text(encoding="utf-8"))


@pytest.mark.parametrize("book", MANIFEST["books"], ids=lambda b: b["file"])
def test_golden_book_passages(book):
    parsed = parse_book(FIXTURES / book["file"], book["book_id"])
    got = [
        {
            "passage_id": p["passage_id"],
            "spine_index": p["spine_index"],
            "kind": p["kind"],
            "block_index": p["block_index"],
            "text": p["text"],
            "seq": p["seq"],
        }
        for p in parsed.passages
    ]
    assert got == book["passages"]
    assert parsed.title == book["title"]
    assert parsed.author == book["author"]
    assert parsed.language == book["language"]
    assert [c.title for c in parsed.chapters] == [c["title"] for c in book["chapters"]]


def test_golden_ids_share_book_prefix():
    for book in MANIFEST["books"]:
        assert all(p["passage_id"].startswith(book["book_id"] + ":") for p in book["passages"])
        assert len({p["passage_id"] for p in book["passages"]}) == len(book["passages"])


def test_golden_manifest_files_exist():
    for book in MANIFEST["books"]:
        data = (FIXTURES / book["file"]).read_bytes()
        assert len(data) > 0
        assert book["sha256"] == __import__("hashlib").sha256(data).hexdigest()


def test_txt_passes_are_line_oriented(tmp_path):
    path = tmp_path / "notes.txt"
    path.write_text("第一行\n\n第二行\n第三行\n", encoding="utf-8")
    parsed = parse_book(path, "abcdef0123456789")
    assert parsed.passage_count == 3
    assert [p["passage_id"] for p in parsed.passages] == [
        "abcdef0123456789:0:p0",
        "abcdef0123456789:0:p1",
        "abcdef0123456789:0:p2",
    ]
    assert [p["block_index"] for p in parsed.passages] == [0, 1, 2]


def test_normalize_cjk_linebreaks():
    assert normalize_text("山间的\n书店") == "山间的书店"
    assert normalize_text("山间的 书店") == "山间的书店"
    assert normalize_text("灯 。") == "灯。"
    assert normalize_text("hello   world") == "hello world"
