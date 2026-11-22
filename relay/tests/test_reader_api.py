"""Reader query API: citation grounding, downgrade, scopes and failures."""

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
PASSAGE = BOOK["passages"][0]


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_LIBRARY_DIR", str(tmp_path / "library"))
    monkeypatch.setenv("BOOK_INGEST_SYNC", "1")
    monkeypatch.setenv("MOCK_LLM_TEXT", "（测试默认非 JSON）")
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


def _query(client, **overrides):
    body = {
        "request_id": "req-1",
        "book_id": BOOK["book_id"],
        "spine_index": 0,
        "passage_ids": [PASSAGE["passage_id"]],
        "question": "这句话在整章里起什么作用？",
        "intent": "explain",
        "context_scope": "current_passage",
    }
    body.update(overrides)
    return client.post("/api/v1/reader/query", headers=DEVICE, json=body)


def _set_llm(monkeypatch, text: str | None = None, fail: bool = False):
    if text is not None:
        monkeypatch.setenv("MOCK_LLM_TEXT", text)
    monkeypatch.setenv("MOCK_LLM_FAIL", "1" if fail else "0")
    reload_settings()


def test_valid_book_citation(client, monkeypatch):
    answer = {
        "segments": [
            {
                "source": "book",
                "text": "开头一句把空间点亮。",
                "citations": [{"passage_id": PASSAGE["passage_id"], "quote": "山间的书店有一盏灯。"}],
            },
            {"source": "inference", "text": "这也是故事的入口。"},
        ]
    }
    _set_llm(monkeypatch, json.dumps(answer, ensure_ascii=False))
    resp = _query(client)
    assert resp.status_code == 200
    body = resp.json()
    assert body["segments"][0]["source"] == "book"
    assert body["segments"][0]["citations"][0]["passage_id"] == PASSAGE["passage_id"]
    assert body["segments"][1]["source"] == "inference"
    assert body["warnings"] == []


def test_forged_quote_downgrades_to_inference(client, monkeypatch):
    answer = {
        "segments": [
            {
                "source": "book",
                "text": "伪造的原文。",
                "citations": [{"passage_id": PASSAGE["passage_id"], "quote": "这句话不在书里"}],
            }
        ]
    }
    _set_llm(monkeypatch, json.dumps(answer, ensure_ascii=False))
    resp = _query(client)
    assert resp.status_code == 200
    body = resp.json()
    assert body["segments"][0]["source"] == "inference"
    assert body["segments"][0]["citations"] == []
    assert "CITATION_DROPPED" in body["warnings"]


def test_non_json_model_output_is_inference(client, monkeypatch):
    _set_llm(monkeypatch, "这是一段普通的自然语言回答，不是 JSON。")
    resp = _query(client)
    assert resp.status_code == 200
    body = resp.json()
    assert body["segments"][0]["source"] == "inference"
    assert "MODEL_OUTPUT_NOT_JSON" in body["warnings"]


def test_current_book_scope_rejected(client):
    resp = _query(client, context_scope="current_book")
    assert resp.status_code == 400
    assert resp.json()["error"]["code"] == "SCOPE_NOT_SUPPORTED"


def test_book_not_ready(client):
    client.app.state.library.update_book_status(BOOK["book_id"], status="pending", progress=0.0)
    resp = _query(client)
    assert resp.status_code == 409
    assert resp.json()["error"]["code"] == "BOOK_NOT_READY"


def test_model_failure(client, monkeypatch):
    _set_llm(monkeypatch, fail=True)
    resp = _query(client)
    assert resp.status_code == 502
    assert resp.json()["error"]["code"] == "MODEL_UNAVAILABLE"


def test_selected_text_too_large(client):
    resp = _query(client, selected_text="字" * 5000)
    assert resp.status_code == 400
    assert resp.json()["error"]["code"] == "BAD_REQUEST"


def test_unknown_passage(client):
    resp = _query(client, passage_ids=[BOOK["book_id"] + ":9:p99"])
    assert resp.status_code == 404
    assert resp.json()["error"]["code"] == "PASSAGE_NOT_FOUND"


def test_missing_context_and_question_is_bad_request(client):
    resp = _query(client, passage_ids=[], question="")
    assert resp.status_code == 400
    assert resp.json()["error"]["code"] == "BAD_REQUEST"
