"""End-to-end V1 chain on golden data.

upload EPUB -> index -> grounded reader query -> sync push -> sync pull
"""

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
FIRST = BOOK["passages"][0]
SECOND = BOOK["passages"][1]


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_LIBRARY_DIR", str(tmp_path / "library"))
    monkeypatch.setenv("BOOK_INGEST_SYNC", "1")
    reload_settings()
    with TestClient(app) as c:
        yield c
    reload_settings()


def test_device_config_advertises_reader_features(client):
    config = client.get("/api/v1/device/config", headers=DEVICE).json()
    assert config["features"]["readerQuery"] is True
    assert config["features"]["bookSync"] is True
    assert config["features"]["knowledgeCards"] is False


def test_upload_index_query_and_sync_round_trip(client, monkeypatch):
    data = (FIXTURES / BOOK["file"]).read_bytes()
    uploaded = client.post(
        "/api/v1/books",
        headers=DEVICE,
        files={"file": (BOOK["file"], data, "application/epub+zip")},
    )
    assert uploaded.status_code == 202
    book_id = uploaded.json()["book_id"]
    assert book_id == BOOK["book_id"]
    assert uploaded.json()["status"] == "ready"

    # 1. Passage IDs served by the API match the frozen manifest exactly.
    served = client.get(
        f"/api/v1/books/{book_id}/passages",
        headers=DEVICE,
        params={"spine_index": 0, "limit": 10},
    ).json()["passages"]
    assert [p["passage_id"] for p in served] == [p["passage_id"] for p in BOOK["passages"][:10]]
    assert served[0]["text"] == FIRST["text"]

    # 2. A grounded answer keeps the book segment and its citation.
    answer = {
        "segments": [
            {
                "source": "book",
                "text": "开头用一盏灯点亮了书店。",
                "citations": [{"passage_id": FIRST["passage_id"], "quote": FIRST["text"]}],
            },
            {"source": "inference", "text": "这为后文的安静氛围定下基调。"},
        ]
    }
    monkeypatch.setenv("MOCK_LLM_TEXT", json.dumps(answer, ensure_ascii=False))
    reload_settings()
    response = client.post(
        "/api/v1/reader/query",
        headers=DEVICE,
        json={
            "request_id": "onepage-s3:00000001",
            "book_id": book_id,
            "spine_index": 0,
            "passage_ids": [FIRST["passage_id"]],
            "selected_text": FIRST["text"],
            "question": "这句话在整章里起什么作用？",
            "intent": "explain",
            "context_scope": "current_chapter",
            "response_style": "concise",
            "language": "zh",
        },
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["segments"][0]["source"] == "book"
    assert body["segments"][0]["citations"][0] == {
        "passage_id": FIRST["passage_id"],
        "spine_index": 0,
        "quote": FIRST["text"],
    }
    assert body["segments"][1]["source"] == "inference"
    assert body["warnings"] == []

    # 3. Save the answer and an annotation, then pull them back.
    push = client.post(
        "/api/v1/sync/push",
        headers=DEVICE,
        json={
            "device_id": "debug-client",
            "batch_id": "batch-e2e",
            "operations": [
                {
                    "op_id": "op-annotation-1",
                    "type": "annotation.create",
                    "client_ts": 1763550000000,
                    "book_id": book_id,
                    "payload": {
                        "id": "an-e2e-1",
                        "book_id": book_id,
                        "passage_id": FIRST["passage_id"],
                        "quote": FIRST["text"],
                        "note": "像故事的入口。",
                        "kind": "highlight",
                        "anchor": {
                            "spine_index": 0,
                            "kind": "p",
                            "block_index": 0,
                            "char_start": served[0]["char_start"],
                            "char_end": served[0]["char_end"],
                        },
                    },
                },
                {
                    "op_id": "op-ai-1",
                    "type": "ai_result.create",
                    "book_id": book_id,
                    "payload": {
                        "id": "ai-e2e-1",
                        "book_id": book_id,
                        "passage_ids": [FIRST["passage_id"], SECOND["passage_id"]],
                        "question": "这句话在整章里起什么作用？",
                        "segments": body["segments"],
                    },
                },
            ],
        },
    )
    assert push.status_code == 200, push.text
    assert [r["status"] for r in push.json()["results"]] == ["applied", "applied"]

    # 4. Retrying the same batch is idempotent.
    retry = client.post(
        "/api/v1/sync/push",
        headers=DEVICE,
        json={
            "device_id": "debug-client",
            "batch_id": "batch-e2e",
            "operations": [
                {
                    "op_id": "op-annotation-1",
                    "type": "annotation.create",
                    "payload": {
                        "id": "an-e2e-1",
                        "book_id": book_id,
                        "passage_id": FIRST["passage_id"],
                        "quote": FIRST["text"],
                        "note": "像故事的入口。",
                        "kind": "highlight",
                    },
                }
            ],
        },
    )
    assert retry.json()["results"][0]["status"] == "duplicate"

    pulled = client.get("/api/v1/sync/pull", headers=DEVICE, params={"device_id": "debug-client"})
    assert pulled.status_code == 200
    changes = pulled.json()["changes"]
    assert [c["entity_type"] for c in changes] == ["annotation", "ai_result"]
    assert changes[0]["entity"]["anchor"]["block_index"] == 0
    assert changes[1]["entity"]["segments"][0]["source"] == "book"
    assert pulled.json()["next_cursor"] == "2"
