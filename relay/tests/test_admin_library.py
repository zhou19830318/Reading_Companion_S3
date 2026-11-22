"""Admin workbench: library upload, status list, notes and soft delete."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from relay.config import reload_settings
from relay.main import app

ADMIN = {"X-Admin-Token": "test-admin-token"}
DEVICE = {"Authorization": "Bearer test-device-token", "X-Device-Id": "debug-client"}
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "golden_books"
MANIFEST = json.loads((FIXTURES / "golden_manifest.json").read_text(encoding="utf-8"))
BOOK = MANIFEST["books"][0]


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_LIBRARY_DIR", str(tmp_path / "library"))
    monkeypatch.setenv("BOOK_INGEST_SYNC", "1")
    reload_settings()
    with TestClient(app) as c:
        yield c
    reload_settings()


def _upload(client, book=BOOK, *, headers=ADMIN, filename=None, data=None):
    payload = (FIXTURES / book["file"]).read_bytes() if data is None else data
    return client.post(
        "/admin/library/upload",
        headers=headers,
        files={"file": (filename or book["file"], payload, "application/epub+zip")},
    )


def test_admin_library_requires_admin_token(client):
    assert client.get("/admin/library").status_code == 403
    assert _upload(client, headers={}).status_code == 403
    assert client.delete("/admin/library/books", params={"bookId": BOOK["book_id"]}).status_code == 403


def test_admin_page_contains_library_sections(client, monkeypatch):
    from relay import admin_page

    monkeypatch.setattr(admin_page, "_is_loopback", lambda request: True)
    page = client.get("/admin")
    assert page.status_code == 200
    assert "阅读书库" in page.text
    assert "笔记与引用" in page.text
    assert "/admin/library/upload" in page.text


def test_admin_upload_lists_book_and_annotations(client):
    resp = _upload(client)
    assert resp.status_code == 201, resp.text
    body = resp.json()
    assert body["ok"] is True
    assert body["duplicate"] is False
    assert body["book"]["book_id"] == BOOK["book_id"]
    assert body["book"]["status"] == "ready"
    assert body["book"]["chapters"] == 2
    assert body["book"]["passages"] == 20

    listing = client.get("/admin/library", headers=ADMIN).json()
    assert [b["book_id"] for b in listing["books"]] == [BOOK["book_id"]]
    assert listing["books"][0]["title"] == BOOK["title"]
    assert listing["annotations"] == []

    # A device-synced annotation shows up read-only on the workbench.
    op = {
        "op_id": "op-admin-view",
        "type": "annotation.create",
        "payload": {
            "id": "an-admin-view",
            "book_id": BOOK["book_id"],
            "passage_id": BOOK["passages"][0]["passage_id"],
            "quote": BOOK["passages"][0]["text"],
            "note": "工作台可见",
            "kind": "highlight",
        },
    }
    assert client.post("/api/v1/sync/push", headers=DEVICE, json={"operations": [op]}).status_code == 200
    listing = client.get("/admin/library", headers=ADMIN).json()
    assert listing["annotations"][0]["note"] == "工作台可见"
    assert listing["annotations"][0]["quote"] == BOOK["passages"][0]["text"]


def test_admin_duplicate_upload_reports_existing(client):
    assert _upload(client).status_code == 201
    again = _upload(client)
    assert again.status_code == 200
    assert again.json()["duplicate"] is True
    assert len(client.get("/admin/library", headers=ADMIN).json()["books"]) == 1


def test_admin_upload_rejects_unsupported_format(client):
    resp = _upload(client, filename="notes.pdf", data=b"%PDF-1.4")
    assert resp.status_code == 415
    assert resp.json()["error"] == "UNSUPPORTED_FORMAT"


def test_admin_soft_delete_hides_book_from_device(client):
    assert _upload(client).status_code == 201
    deleted = client.delete(
        "/admin/library/books", headers=ADMIN, params={"bookId": BOOK["book_id"]}
    ).json()
    assert deleted["deleted"] is True
    assert client.get("/admin/library", headers=ADMIN).json()["books"] == []
    assert client.get(f"/api/v1/books/{BOOK['book_id']}", headers=DEVICE).status_code == 404
    assert client.get(f"/api/v1/books/by-hash/{BOOK['sha256']}", headers=DEVICE).status_code == 404
    # Deleting again is a no-op, not an error.
    again = client.delete(
        "/admin/library/books", headers=ADMIN, params={"bookId": BOOK["book_id"]}
    ).json()
    assert again["deleted"] is False
