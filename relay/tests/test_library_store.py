"""Library store: ingest, FTS, neighbors and idempotent sync operations."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

from relay.book_ingest import ingest_book
from relay.library_store import LibraryStore

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "golden_books"
MANIFEST = json.loads((FIXTURES / "golden_manifest.json").read_text(encoding="utf-8"))
RELAY_ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture()
def store(tmp_path):
    s = LibraryStore(tmp_path / "library.db", tmp_path / "books")
    s.open()
    yield s
    s.close()


def _ingest(s: LibraryStore, book: dict) -> dict:
    data = (FIXTURES / book["file"]).read_bytes()
    row = s.create_book(
        sha256=book["sha256"],
        title="upload",
        author="",
        language="",
        book_format="epub",
        size=len(data),
        file_path=str(FIXTURES / book["file"]),
    )
    ingest_book(s, row["book_id"], FIXTURES / book["file"])
    return s.get_book(row["book_id"])


def test_ingest_counts_and_metadata(store):
    book = _ingest(store, MANIFEST["books"][0])
    assert book["status"] == "ready"
    assert book["chapters"] == 2
    assert book["passages"] == 20
    assert book["title"] == "山间书店"
    assert [c["title"] for c in store.list_chapters(book["book_id"])] == ["第一章 山间", "第二章 灯下"]


def test_fts_and_short_cjk_fallback(store):
    book = _ingest(store, MANIFEST["books"][0])
    found = store.search(book["book_id"], "书店有一盏灯")
    assert found and "山间的书店有一盏灯" in found[0]["text"]
    # 1-2 char CJK is below the trigram minimum; LIKE must still find it.
    short = store.search(book["book_id"], "灯")
    assert short and any("灯" in p["text"] for p in short)


def test_neighbors_and_chapter(store):
    book = _ingest(store, MANIFEST["books"][0])
    passages = MANIFEST["books"][0]["passages"]
    first = passages[0]["passage_id"]
    neighbors = store.passage_neighbors(book["book_id"], first, radius=1)
    assert [p["passage_id"] for p in neighbors] == [first, passages[1]["passage_id"]]
    chapter = store.chapter_passages(book["book_id"], 0)
    assert len(chapter) == 10
    assert chapter[0]["text"].startswith("山间的书店")


def test_fts_rebuild_tool(store):
    book = _ingest(store, MANIFEST["books"][0])
    result = subprocess.run(
        [sys.executable, "tools/rebuild_fts.py", "--db", str(store.db_path)],
        cwd=RELAY_ROOT,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    assert "rebuilt passages_fts" in result.stdout
    assert store.search(book["book_id"], "书店有一盏灯")


def test_duplicate_hash_returns_existing(store):
    book = _ingest(store, MANIFEST["books"][0])
    same = store.get_book_by_hash(MANIFEST["books"][0]["sha256"])
    assert same["book_id"] == book["book_id"]
    # create_book is idempotent by hash.
    again = store.create_book(
        sha256=MANIFEST["books"][0]["sha256"],
        title="other",
        author="",
        language="",
        book_format="epub",
        size=1,
        file_path="x",
    )
    assert again["book_id"] == book["book_id"]
    assert again["title"] == book["title"]


def test_sync_annotation_idempotency_and_pull(store):
    op = {
        "op_id": "op-1",
        "type": "annotation.create",
        "payload": {
            "id": "an-1",
            "book_id": "book-1",
            "passage_id": "book-1:0:p0",
            "quote": "山间的书店有一盏灯。",
            "note": "入口",
            "kind": "highlight",
            "anchor": {"spine_index": 0, "kind": "p", "block_index": 0},
        },
    }
    assert store.apply_sync_operation("dev-1", op)["status"] == "applied"
    assert store.apply_sync_operation("dev-1", op)["status"] == "duplicate"

    pulled = store.pull_changes("dev-1", 0, 10)
    assert pulled["has_more"] is False
    assert len(pulled["changes"]) == 1
    assert pulled["changes"][0]["entity"]["note"] == "入口"
    assert pulled["next_cursor"] == "1"

    delete = {"op_id": "op-2", "type": "annotation.delete", "payload": {"id": "an-1"}}
    assert store.apply_sync_operation("dev-1", delete)["status"] == "applied"
    assert store.apply_sync_operation("dev-1", delete)["status"] == "duplicate"
    deleted = store.pull_changes("dev-1", 1, 10)["changes"][0]["entity"]
    assert deleted["deleted"] is True


def test_sync_conflict_is_not_silently_overwritten(store):
    base = {
        "type": "annotation.create",
        "payload": {
            "id": "an-2",
            "book_id": "book-1",
            "passage_id": "book-1:0:p1",
            "quote": "原文",
            "note": "v1",
            "kind": "highlight",
        },
    }
    assert store.apply_sync_operation("dev-1", {"op_id": "op-a", **base})["status"] == "applied"
    second = {
        "op_id": "op-b",
        "type": "annotation.create",
        "payload": {**base["payload"], "note": "v2"},
    }
    assert store.apply_sync_operation("dev-1", second)["status"] == "conflict"
    # A retry must keep returning conflict, not turn into an ackable duplicate.
    assert store.apply_sync_operation("dev-1", second)["status"] == "conflict"
    assert store.get_annotation("an-2")["note"] == "v1"


def test_sync_ai_result(store):
    op = {
        "op_id": "op-ai",
        "type": "ai_result.create",
        "payload": {
            "id": "ai-1",
            "book_id": "book-1",
            "passage_ids": ["book-1:0:p0"],
            "question": "解释",
            "segments": [{"source": "book", "text": "答案", "citations": []}],
        },
    }
    assert store.apply_sync_operation("dev-1", op)["status"] == "applied"
    assert store.apply_sync_operation("dev-1", op)["status"] == "duplicate"
    pulled = store.pull_changes("dev-1", 0, 10)
    assert pulled["changes"][0]["entity_type"] == "ai_result"
    assert pulled["changes"][0]["entity"]["question"] == "解释"


def test_sync_rejects_unknown_type(store):
    result = store.apply_sync_operation("dev-1", {"op_id": "op-x", "type": "nope", "payload": {}})
    assert result["status"] == "rejected"
    assert "unknown type" in result["error"]


def test_sync_rejects_cross_device_mutation(store):
    op = {
        "op_id": "op-owner",
        "type": "annotation.create",
        "payload": {
            "id": "an-owner",
            "book_id": "book-1",
            "passage_id": "book-1:0:p0",
            "quote": "原文",
            "note": "owner",
            "kind": "highlight",
        },
    }
    assert store.apply_sync_operation("dev-1", op)["status"] == "applied"
    steal = {"op_id": "op-steal", "type": "annotation.create", "payload": {**op["payload"], "note": "other"}}
    assert store.apply_sync_operation("dev-2", steal)["status"] == "rejected"
    delete = {"op_id": "op-steal-del", "type": "annotation.delete", "payload": {"id": "an-owner"}}
    assert store.apply_sync_operation("dev-2", delete)["status"] == "rejected"
    assert store.get_annotation("an-owner")["note"] == "owner"
    assert store.get_annotation("an-owner")["deleted"] is False


def test_persistence_failure_marks_book_failed(store, monkeypatch):
    """A DB/disk error after parsing must not leave the book stuck in indexing."""
    def boom(*args, **kwargs):
        raise RuntimeError("disk I/O error")

    monkeypatch.setattr(store, "replace_book_content", boom)
    book = _ingest(store, MANIFEST["books"][0])
    assert book["status"] == "failed"
    assert book["error"]["code"] == "INTERNAL_ERROR"
    assert "disk I/O error" in book["error"]["message"]


def test_passage_cap_marks_book_failed(store, monkeypatch):
    from relay import book_ingest

    monkeypatch.setattr(book_ingest, "MAX_BOOK_PASSAGES", 5)
    book = _ingest(store, MANIFEST["books"][0])
    assert book["status"] == "failed"
    assert book["error"]["code"] == "TOO_MANY_PASSAGES"
