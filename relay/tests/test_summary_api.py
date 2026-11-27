"""Cached chapter summary and whole-book introduction API."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from relay.config import reload_settings
from relay.main import app

DEVICE = {"Authorization": "Bearer test-device-token", "X-Device-Id": "debug-client"}
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "golden_books"
MANIFEST = json.loads((FIXTURES / "golden_manifest.json").read_text(encoding="utf-8"))
BOOK = MANIFEST["books"][0]


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_LIBRARY_DIR", str(tmp_path / "library"))
    monkeypatch.setenv("BOOK_INGEST_SYNC", "1")
    monkeypatch.setenv("MOCK_LLM_TEXT", "本章讲述山间书店的故事，并引出主人公的旅程。")
    reload_settings()
    with TestClient(app) as c:
        data = (FIXTURES / BOOK["file"]).read_bytes()
        resp = c.post(
            "/api/v1/books",
            headers=DEVICE,
            files={"file": (BOOK["file"], data, "application/epub+zip")},
        )
        assert resp.status_code == 202
        yield c
    reload_settings()


def _summary(client, **params):
    return client.get(f"/api/v1/books/{BOOK['book_id']}/summary", headers=DEVICE, params=params)


def test_chapter_summary_is_cached(client, monkeypatch):
    first = _summary(client, scope="chapter", spine_index=0)
    assert first.status_code == 200
    body = first.json()
    assert body["status"] == "ready"
    assert body["scope"] == "chapter"
    assert body["spine_index"] == 0
    assert body["cached"] is False
    assert "山间书店" in body["text"]

    monkeypatch.setenv("MOCK_LLM_FAIL", "1")
    reload_settings()
    second = _summary(client, scope="chapter", spine_index=0)
    assert second.status_code == 200
    assert second.json()["cached"] is True
    assert second.json()["cache_key"] == body["cache_key"]


def test_book_intro(client):
    resp = _summary(client, scope="book")
    assert resp.status_code == 200
    body = resp.json()
    assert body["scope"] == "book"
    assert body["spine_index"] is None
    assert body["status"] == "ready"
    assert body["text"]


def test_invalid_scope_and_missing_spine(client):
    assert _summary(client, scope="unknown").json()["error"]["code"] == "BAD_REQUEST"
    assert _summary(client, scope="chapter").json()["error"]["code"] == "BAD_REQUEST"


def test_model_failure_is_not_cached(client, monkeypatch):
    monkeypatch.setenv("MOCK_LLM_FAIL", "1")
    reload_settings()
    resp = _summary(client, scope="chapter", spine_index=0)
    assert resp.status_code == 502
    assert resp.json()["error"]["code"] == "MODEL_UNAVAILABLE"
