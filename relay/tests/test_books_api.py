"""Book upload / metadata / passage APIs against the golden EPUBs."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from relay import book_ingest
from relay.config import reload_settings
from relay.main import app

DEVICE = {"Authorization": "Bearer test-device-token", "X-Device-Id": "debug-client"}
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "golden_books"
MANIFEST = json.loads((FIXTURES / "golden_manifest.json").read_text(encoding="utf-8"))


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_LIBRARY_DIR", str(tmp_path / "library"))
    monkeypatch.setenv("BOOK_INGEST_SYNC", "1")
    reload_settings()
    with TestClient(app) as c:
        yield c
    reload_settings()


def _upload(client, book):
    data = (FIXTURES / book["file"]).read_bytes()
    return client.post(
        "/api/v1/books",
        headers=DEVICE,
        files={"file": (book["file"], data, "application/epub+zip")},
    )


def test_books_require_device_token(client):
    assert client.get("/api/v1/books").status_code == 401
    # Upload authenticates before parsing the multipart body.
    assert client.post("/api/v1/books").status_code == 401
    assert (
        client.post(
            "/api/v1/reader/query",
            json={"request_id": "r", "book_id": "b"},
        ).status_code
        == 401
    )
    assert client.post("/api/v1/sync/push", json={"operations": []}).status_code == 401
    assert client.get("/api/v1/sync/pull").status_code == 401


def test_upload_and_query_metadata(client):
    book = MANIFEST["books"][0]
    resp = _upload(client, book)
    assert resp.status_code == 202
    body = resp.json()
    assert body["book_id"] == book["book_id"]
    assert body["sha256"] == book["sha256"]
    assert body["status"] == "ready"
    assert body["duplicate"] is False

    by_hash = client.get(f"/api/v1/books/by-hash/{book['sha256']}", headers=DEVICE)
    assert by_hash.status_code == 200
    assert by_hash.json()["title"] == book["title"]

    processing = client.get(f"/api/v1/books/{book['book_id']}/processing", headers=DEVICE).json()
    assert processing["status"] == "ready"
    assert processing["chapters"] == 2
    assert processing["passages"] == 20

    chapters = client.get(f"/api/v1/books/{book['book_id']}/chapters", headers=DEVICE).json()["chapters"]
    assert [c["title"] for c in chapters] == ["第一章 山间", "第二章 灯下"]

    passages = client.get(
        f"/api/v1/books/{book['book_id']}/passages", headers=DEVICE, params={"spine_index": 0, "limit": 3}
    ).json()
    assert [p["passage_id"] for p in passages["passages"]] == [
        p["passage_id"] for p in book["passages"][:3]
    ]
    assert passages["next"] is not None


def test_duplicate_upload_is_idempotent(client):
    book = MANIFEST["books"][1]
    assert _upload(client, book).status_code == 202
    again = _upload(client, book)
    assert again.status_code == 200
    assert again.json()["duplicate"] is True
    assert again.json()["book_id"] == book["book_id"]
    listing = client.get("/api/v1/books", headers=DEVICE).json()["books"]
    assert len([b for b in listing if b["book_id"] == book["book_id"]]) == 1


def test_duplicate_upload_retries_failed_ingest(client, monkeypatch):
    book = MANIFEST["books"][0]
    real_parse = book_ingest.parse_book
    attempts = 0

    def fail_once(path, book_id):
        nonlocal attempts
        attempts += 1
        if attempts == 1:
            raise book_ingest.IngestError("EPUB_PARSE_FAILED", "temporary parser failure")
        return real_parse(path, book_id)

    monkeypatch.setattr(book_ingest, "parse_book", fail_once)
    first = _upload(client, book)
    assert first.status_code == 202
    assert first.json()["status"] == "failed"

    retry = _upload(client, book)
    assert retry.status_code == 200
    assert retry.json()["duplicate"] is True
    assert retry.json()["status"] == "ready"
    processing = client.get(f"/api/v1/books/{book['book_id']}/processing", headers=DEVICE).json()
    assert processing["status"] == "ready"
    assert processing["passages"] == 20
    assert attempts == 2


def test_unsupported_format(client, tmp_path):
    resp = client.post(
        "/api/v1/books",
        headers=DEVICE,
        files={"file": ("book.pdf", b"%PDF-1.4", "application/pdf")},
    )
    assert resp.status_code == 415
    assert resp.json()["error"]["code"] == "UNSUPPORTED_FORMAT"


def test_missing_book(client):
    assert client.get("/api/v1/books/nope", headers=DEVICE).status_code == 404
    assert client.get("/api/v1/books/by-hash/" + "0" * 64, headers=DEVICE).status_code == 404


def test_corrupt_epub_reports_failed_status(client):
    resp = client.post(
        "/api/v1/books",
        headers=DEVICE,
        files={"file": ("broken.epub", b"this is not a zip", "application/epub+zip")},
    )
    assert resp.status_code == 202
    book_id = resp.json()["book_id"]
    processing = client.get(f"/api/v1/books/{book_id}/processing", headers=DEVICE).json()
    assert processing["status"] == "failed"
    assert processing["error"]["code"] == "EPUB_PARSE_FAILED"
    assert processing["passages"] == 0


def test_empty_upload_rejected(client):
    resp = client.post(
        "/api/v1/books",
        headers=DEVICE,
        files={"file": ("empty.epub", b"", "application/epub+zip")},
    )
    assert resp.status_code == 400
    assert resp.json()["error"]["code"] == "BAD_REQUEST"


def test_oversize_upload_rejected(client, monkeypatch):
    monkeypatch.setenv("MAX_BOOK_BYTES", "16")
    reload_settings()
    try:
        resp = client.post(
            "/api/v1/books",
            headers=DEVICE,
            files={"file": ("big.txt", b"0123456789abcdefXYZ", "text/plain")},
        )
        assert resp.status_code == 413
        assert resp.json()["error"]["code"] == "BOOK_TOO_LARGE"
    finally:
        monkeypatch.setenv("MAX_BOOK_BYTES", str(64 * 1024 * 1024))
        reload_settings()


def test_txt_upload_is_line_indexed(client):
    resp = client.post(
        "/api/v1/books",
        headers=DEVICE,
        files={"file": ("notes.txt", "第一行\n\n第二行\n".encode(), "text/plain")},
    )
    assert resp.status_code == 202
    body = resp.json()
    assert body["status"] == "ready"
    book_id = body["book_id"]
    passages = client.get(f"/api/v1/books/{book_id}/passages", headers=DEVICE).json()["passages"]
    assert [p["text"] for p in passages] == ["第一行", "第二行"]


def test_background_ingest_path_reaches_ready(client, monkeypatch):
    # Production default: response is pending, parsing runs after the response.
    monkeypatch.setenv("BOOK_INGEST_SYNC", "0")
    reload_settings()
    try:
        book = MANIFEST["books"][2]
        resp = _upload(client, book)
        assert resp.status_code == 202
        assert resp.json()["status"] in ("pending", "parsing")
        processing = client.get(
            f"/api/v1/books/{book['book_id']}/processing", headers=DEVICE
        ).json()
        assert processing["status"] == "ready"
        assert processing["passages"] == 20
    finally:
        monkeypatch.setenv("BOOK_INGEST_SYNC", "1")
        reload_settings()


ADMIN = {"X-Admin-Token": "test-admin-token"}


def test_reupload_after_soft_delete_restores_book(client):
    """The hash row survives a soft delete; re-upload must restore, not 500."""
    book = MANIFEST["books"][0]
    assert _upload(client, book).status_code == 202
    deleted = client.delete(
        "/admin/library/books", headers=ADMIN, params={"bookId": book["book_id"]}
    )
    assert deleted.status_code == 200 and deleted.json()["deleted"] is True
    assert client.get(f"/api/v1/books/{book['book_id']}", headers=DEVICE).status_code == 404

    restored = _upload(client, book)
    assert restored.status_code == 202, restored.text
    assert restored.json()["duplicate"] is False
    assert restored.json()["status"] == "ready"
    assert client.get(f"/api/v1/books/by-hash/{book['sha256']}", headers=DEVICE).status_code == 200
    served = client.get(
        f"/api/v1/books/{book['book_id']}/passages", headers=DEVICE, params={"spine_index": 0}
    ).json()["passages"]
    assert [p["passage_id"] for p in served] == [
        p["passage_id"] for p in book["passages"] if p["spine_index"] == 0
    ]


def test_deleted_book_is_not_queryable(client):
    book = MANIFEST["books"][0]
    assert _upload(client, book).status_code == 202
    assert client.delete(
        "/admin/library/books", headers=ADMIN, params={"bookId": book["book_id"]}
    ).status_code == 200
    resp = client.post(
        "/api/v1/reader/query",
        headers=DEVICE,
        json={
            "request_id": "deleted-book",
            "book_id": book["book_id"],
            "passage_ids": [book["passages"][0]["passage_id"]],
            "question": "还在吗？",
        },
    )
    assert resp.status_code == 404
    assert resp.json()["error"]["code"] == "BOOK_NOT_FOUND"
