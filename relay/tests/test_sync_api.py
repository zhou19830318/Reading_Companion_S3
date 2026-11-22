"""Sync API: idempotent push, cursor pull, conflicts and device checks."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from relay.config import reload_settings
from relay.main import app

DEVICE = {"Authorization": "Bearer test-device-token", "X-Device-Id": "debug-client"}


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_LIBRARY_DIR", str(tmp_path / "library"))
    reload_settings()
    with TestClient(app) as c:
        yield c
    reload_settings()


def _annotation_op(op_id: str = "op-1", note: str = "v1", entity_id: str = "an-1") -> dict:
    return {
        "op_id": op_id,
        "type": "annotation.create",
        "client_ts": 1763550000000,
        "book_id": "book-1",
        "payload": {
            "id": entity_id,
            "book_id": "book-1",
            "passage_id": "book-1:0:p0",
            "quote": "山间的书店有一盏灯。",
            "note": note,
            "kind": "highlight",
            "anchor": {"spine_index": 0, "kind": "p", "block_index": 0},
        },
    }


def _push(client, operations, **overrides):
    body = {"device_id": "debug-client", "batch_id": "batch-1", "operations": operations}
    body.update(overrides)
    return client.post("/api/v1/sync/push", headers=DEVICE, json=body)


def test_push_applies_and_pull_returns_change(client):
    resp = _push(client, [_annotation_op()])
    assert resp.status_code == 200
    result = resp.json()["results"][0]
    assert result["status"] == "applied"
    assert result["server_id"] == "an-1"

    pulled = client.get("/api/v1/sync/pull", headers=DEVICE, params={"device_id": "debug-client"})
    assert pulled.status_code == 200
    body = pulled.json()
    assert body["has_more"] is False
    assert body["next_cursor"] == "1"
    assert body["changes"][0]["entity_type"] == "annotation"
    assert body["changes"][0]["entity"]["note"] == "v1"
    assert body["changes"][0]["entity"]["anchor"]["block_index"] == 0


def test_repeating_op_id_is_duplicate_and_does_not_duplicate_rows(client):
    assert _push(client, [_annotation_op()]).json()["results"][0]["status"] == "applied"
    again = _push(client, [_annotation_op(note="changed")])
    assert again.json()["results"][0]["status"] == "duplicate"
    changes = client.get("/api/v1/sync/pull", headers=DEVICE).json()["changes"]
    assert len(changes) == 1


def test_same_entity_different_op_conflicts(client):
    assert _push(client, [_annotation_op(op_id="op-a", note="v1")]).json()["results"][0]["status"] == "applied"
    conflict = _push(client, [_annotation_op(op_id="op-b", note="v2")]).json()["results"][0]
    assert conflict["status"] == "conflict"
    # Retrying the conflicting op keeps the decision; it is not an ack.
    retry = _push(client, [_annotation_op(op_id="op-b", note="v2")]).json()["results"][0]
    assert retry["status"] == "conflict"
    changes = client.get("/api/v1/sync/pull", headers=DEVICE).json()["changes"]
    assert changes[0]["entity"]["note"] == "v1"


def test_delete_marks_entity_and_pull_reports_tombstone(client):
    _push(client, [_annotation_op()])
    deleted = _push(
        client,
        [{"op_id": "op-del", "type": "annotation.delete", "payload": {"id": "an-1"}}],
    ).json()["results"][0]
    assert deleted["status"] == "applied"
    changes = client.get("/api/v1/sync/pull", headers=DEVICE, params={"cursor": "1"}).json()["changes"]
    assert changes[0]["op"] == "delete"
    assert changes[0]["entity"]["deleted"] is True


def test_ai_result_create_and_pull(client):
    op = {
        "op_id": "op-ai",
        "type": "ai_result.create",
        "payload": {
            "id": "ai-1",
            "book_id": "book-1",
            "passage_ids": ["book-1:0:p0"],
            "question": "这句话起什么作用？",
            "segments": [
                {
                    "source": "book",
                    "text": "开头把空间点亮。",
                    "citations": [{"passage_id": "book-1:0:p0", "spine_index": 0, "quote": "山间的书店有一盏灯。"}],
                }
            ],
        },
    }
    assert _push(client, [op]).json()["results"][0]["status"] == "applied"
    change = client.get("/api/v1/sync/pull", headers=DEVICE).json()["changes"][0]
    assert change["entity_type"] == "ai_result"
    entity = change["entity"]
    assert entity["question"] == "这句话起什么作用？"
    assert entity["segments"][0]["source"] == "book"
    assert entity["passage_ids"] == ["book-1:0:p0"]


def test_unknown_type_is_rejected_and_kept_for_retry(client):
    op = {"op_id": "op-x", "type": "chapter.rename", "payload": {}}
    first = _push(client, [op]).json()["results"][0]
    assert first["status"] == "rejected"
    assert "unknown type" in first["error"]
    retry = _push(client, [op]).json()["results"][0]
    assert retry["status"] == "rejected"


def test_partial_batch_returns_per_operation_results(client):
    results = _push(
        client,
        [
            _annotation_op("op-ok"),
            {"op_id": "op-bad", "type": "chapter.rename", "payload": {}},
            {
                "op_id": "op-missing-id",
                "type": "annotation.create",
                "payload": {"book_id": "book-1", "passage_id": "book-1:0:p0"},
            },
        ],
    ).json()["results"]
    assert [r["status"] for r in results] == ["applied", "rejected", "rejected"]
    assert results[1]["error"]
    assert results[2]["error"] == "missing payload.id"


def test_device_id_mismatch_is_forbidden(client):
    resp = _push(client, [_annotation_op()], device_id="someone-else")
    assert resp.status_code == 403
    assert resp.json()["error"]["code"] == "DEVICE_NOT_ALLOWED"
    pulled = client.get(
        "/api/v1/sync/pull", headers=DEVICE, params={"device_id": "someone-else"}
    )
    assert pulled.status_code == 403


def test_empty_operations_rejected(client):
    resp = _push(client, [])
    assert resp.status_code == 400
    assert resp.json()["error"]["code"] == "BAD_REQUEST"


def test_batch_limit(client, monkeypatch):
    monkeypatch.setenv("SYNC_BATCH_MAX_OPS", "2")
    reload_settings()
    try:
        assert _push(client, [_annotation_op("op-1"), _annotation_op("op-2", entity_id="an-2")]).status_code == 200
        resp = _push(
            client,
            [
                _annotation_op("op-3", entity_id="an-3"),
                _annotation_op("op-4", entity_id="an-4"),
                _annotation_op("op-5", entity_id="an-5"),
            ],
        )
        assert resp.status_code == 400
        assert "too many operations" in resp.json()["error"]["message"]
    finally:
        monkeypatch.setenv("SYNC_BATCH_MAX_OPS", "50")
        reload_settings()


def test_pull_cursor_is_exclusive_and_paginates(client):
    for i in range(3):
        assert (
            _push(client, [_annotation_op(f"op-{i}", entity_id=f"an-{i}")]).json()["results"][0]["status"]
            == "applied"
        )
    first = client.get("/api/v1/sync/pull", headers=DEVICE, params={"limit": 2}).json()
    assert [c["seq"] for c in first["changes"]] == [1, 2]
    assert first["has_more"] is True
    second = client.get(
        "/api/v1/sync/pull", headers=DEVICE, params={"cursor": first["next_cursor"], "limit": 2}
    ).json()
    assert [c["seq"] for c in second["changes"]] == [3]
    assert second["has_more"] is False


def test_pull_rejects_bad_cursor(client):
    resp = client.get("/api/v1/sync/pull", headers=DEVICE, params={"cursor": "nope"})
    assert resp.status_code == 400
    assert resp.json()["error"]["code"] == "BAD_REQUEST"


def test_pull_only_returns_own_device_changes(client):
    _push(client, [_annotation_op()])
    other = client.get("/api/v1/sync/pull", headers=DEVICE, params={})
    assert other.json()["changes"]  # authenticated debug-client sees its own
    # Another allowed device would not pass check_device in this test setup, so
    # cross-device visibility is covered at the store layer instead.
