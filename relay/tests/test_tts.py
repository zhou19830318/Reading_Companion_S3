"""Text-to-speech: text preparation, segmentation, pipelining and the WS route.

edge-tts and the MP3 decoder are stubbed. That is deliberate: the tests must
pin the device-facing contract (frame size, frame order, error codes, truncation
flag) without depending on Microsoft's service or on an audio codec.
"""

from __future__ import annotations

import asyncio
import json

import pytest
from fastapi.testclient import TestClient

from relay import tts
from relay.config import get_settings, reload_settings
from relay.main import app

HEADERS = {
    "Authorization": "Bearer test-device-token",
    "X-Device-Id": "debug-client",
    "X-Voice-Protocol": "1",
}


@pytest.fixture()
def client():
    reload_settings()
    with TestClient(app) as test_client:
        yield test_client


# ------------------------------------------------------------------ text shaping


def test_normalize_collapses_layout_breaks_and_drops_controls():
    assert tts.normalize_text("春眠不觉晓\n处处闻啼鸟") == "春眠不觉晓 处处闻啼鸟"
    # A break after sentence punctuation is not turned into a second space.
    assert tts.normalize_text("春风。\n又绿江南岸。") == "春风。又绿江南岸。"
    assert tts.normalize_text("a\tb\r\nc") == "a b c"
    assert tts.normalize_text("  \n  ") == ""
    # Non-printing control characters carry no speech and must not become gaps.
    assert tts.normalize_text("bell\x07here") == "bellhere"


def test_segment_text_prefers_sentence_then_clause_breaks():
    text = "春风又绿江南岸。" * 4
    segments = tts.segment_text(text, limit=10)
    assert all(len(segment) <= 10 for segment in segments)
    # Every segment but the last ends at a sentence mark: the split is natural.
    assert all(segment.endswith("。") for segment in segments[:-1])
    assert "".join(segments) == text


def test_segment_text_falls_back_to_clauses_then_hard_cuts():
    long_sentence = "一二三四五，六七八九十，" + "甲乙丙丁戊己庚辛壬癸"
    segments = tts.segment_text(long_sentence, limit=12)
    assert all(len(segment) <= 12 for segment in segments)
    # No character is lost by any break strategy.
    assert "".join(segments).replace(" ", "") == long_sentence.replace(" ", "")

    unbroken = "字" * 40
    assert [len(segment) for segment in tts.segment_text(unbroken, limit=12)] == [12, 12, 12, 4]


def test_segment_text_handles_empty_input():
    assert tts.segment_text("   ") == []


def test_prepare_reports_truncation():
    settings = get_settings()
    short, truncated = tts.prepare("短句。", settings)
    assert (short, truncated) == ("短句。", False)

    long_text = "字" * (settings.tts_max_chars + 50)
    capped, truncated = tts.prepare(long_text, settings)
    assert truncated is True
    assert len(capped) == settings.tts_max_chars


def test_voice_selection_follows_the_card_language():
    settings = get_settings()
    assert tts.voice_for(settings, "zh") == settings.tts_voice_zh
    assert tts.voice_for(settings, "EN") == settings.tts_voice_en
    assert tts.voice_for(settings, "en-US") == settings.tts_voice_en
    assert tts.voice_for(settings, "") == settings.tts_voice_zh


# --------------------------------------------------------------------- pipeline


def _stub_synthesis(monkeypatch, pcm_by_text: dict[str, bytes], fail_on: set[str] | None = None):
    fail_on = fail_on or set()

    async def fake_synth(edge_tts_module, text, voice, timeout):
        if text in fail_on:
            raise tts.TtsError("upstream_failed")
        return b"mp3:" + text.encode()

    def fake_decode(miniaudio_module, mp3):
        text = mp3.decode().removeprefix("mp3:")
        return pcm_by_text.get(text, b"\x00\x01" * 16)

    monkeypatch.setattr(tts, "_synthesize_mp3", fake_synth)
    monkeypatch.setattr(tts, "_decode_pcm", fake_decode)


def _collect(text: str) -> list[bytes]:
    settings = get_settings()

    async def run() -> list[bytes]:
        return [frame async for frame in tts.pcm_frames(text, settings.tts_voice_zh, settings)]

    return asyncio.run(run())


def test_frames_are_capped_and_carry_every_sample(monkeypatch):
    pcm = bytes(range(256)) * 200  # 51,200 bytes: 6 full frames + a tail
    _stub_synthesis(monkeypatch, {"短句。": pcm})

    frames = _collect("短句。")
    assert len(frames) == 7
    assert all(len(frame) <= tts.FRAME_BYTES for frame in frames)
    assert b"".join(frames) == pcm


def test_segments_are_concatenated_in_order(monkeypatch):
    first, second = "一二三四五六七八九十。", "甲乙丙丁戊己庚辛壬癸。"
    monkeypatch.setenv("TTS_SEGMENT_CHARS", "20")  # the configured minimum
    reload_settings()
    try:
        _stub_synthesis(monkeypatch, {first: b"\x01\x01" * 4, second: b"\x02\x02" * 4})
        frames = _collect(first + second)
        assert b"".join(frames) == b"\x01\x01" * 4 + b"\x02\x02" * 4
    finally:
        monkeypatch.delenv("TTS_SEGMENT_CHARS", raising=False)
        reload_settings()


def test_a_producer_failure_reaches_the_consumer(monkeypatch):
    first, second = "一二三四五六七八九十。", "甲乙丙丁戊己庚辛壬癸。"
    monkeypatch.setenv("TTS_SEGMENT_CHARS", "20")
    reload_settings()
    try:
        _stub_synthesis(monkeypatch, {first: b"\x01\x01" * 4}, fail_on={second})

        async def run():
            settings = get_settings()
            return [frame async for frame in tts.pcm_frames(first + second, settings.tts_voice_zh, settings)]

        # The first segment is delivered before the failure surfaces: a page
        # that dies halfway must not look like a clean, finished stream.
        with pytest.raises(tts.TtsError) as excinfo:
            asyncio.run(run())
        assert excinfo.value.code == "upstream_failed"
    finally:
        monkeypatch.delenv("TTS_SEGMENT_CHARS", raising=False)
        reload_settings()


def test_empty_text_is_rejected(monkeypatch):
    _stub_synthesis(monkeypatch, {})
    with pytest.raises(tts.TtsError) as excinfo:
        _collect("   ")
    assert excinfo.value.code == "bad_text"


# ------------------------------------------------------------------------- route


def test_ws_requires_a_device_token(client):
    with pytest.raises(Exception):
        with client.websocket_connect("/api/v1/tts") as ws:
            ws.receive()


def test_ws_streams_meta_frames_and_end(client, monkeypatch):
    pcm = b"\x10\x20" * 5000  # 10,000 bytes -> 2 frames
    _stub_synthesis(monkeypatch, {"春风又绿江南岸。": pcm})

    with client.websocket_connect("/api/v1/tts", headers=HEADERS) as ws:
        ws.send_text(json.dumps({"type": "tts.start", "id": 7, "text": "春风又绿江南岸。", "lang": "zh"}))

        meta = ws.receive_json()
        assert meta["type"] == "tts.meta"
        assert meta["id"] == 7
        assert (meta["rate"], meta["ch"], meta["bits"]) == (tts.SAMPLE_RATE, 1, 16)
        assert meta["voice"] == get_settings().tts_voice_zh
        assert meta["truncated"] is False

        audio = bytearray()
        while True:
            message = ws.receive()
            if message.get("text") is not None:
                end = json.loads(message["text"])
                break
            audio.extend(message["bytes"])

        assert end["type"] == "tts.end"
        assert end["id"] == 7
        # 10,000 bytes at 16 kHz mono is 312 ms.
        assert end["ms"] == 312
        assert bytes(audio) == pcm


def test_ws_reports_bad_text(client, monkeypatch):
    _stub_synthesis(monkeypatch, {})
    with client.websocket_connect("/api/v1/tts", headers=HEADERS) as ws:
        ws.send_text(json.dumps({"type": "tts.start", "id": 3, "text": "   "}))
        error = ws.receive_json()
        assert error == {"type": "tts.error", "id": 3, "code": "bad_text"}


def test_ws_reports_an_upstream_failure(client, monkeypatch):
    _stub_synthesis(monkeypatch, {}, fail_on={"春风又绿江南岸。"})
    with client.websocket_connect("/api/v1/tts", headers=HEADERS) as ws:
        ws.send_text(json.dumps({"type": "tts.start", "id": 4, "text": "春风又绿江南岸。", "lang": "zh"}))
        assert ws.receive_json()["type"] == "tts.meta"
        error = ws.receive_json()
        assert error == {"type": "tts.error", "id": 4, "code": "upstream_failed"}


def test_ws_rejects_an_unknown_event(client, monkeypatch):
    _stub_synthesis(monkeypatch, {})
    with client.websocket_connect("/api/v1/tts", headers=HEADERS) as ws:
        ws.send_text(json.dumps({"type": "session.start", "id": 5}))
        assert ws.receive_json() == {"type": "tts.error", "id": 5, "code": "bad_request"}


def test_second_request_reuses_the_connection(client, monkeypatch):
    _stub_synthesis(monkeypatch, {"一。": b"\x01\x01", "二。": b"\x02\x02"})
    with client.websocket_connect("/api/v1/tts", headers=HEADERS) as ws:
        for request_id, text in ((1, "一。"), (2, "二。")):
            ws.send_text(json.dumps({"type": "tts.start", "id": request_id, "text": text}))
            assert ws.receive_json()["id"] == request_id
            while True:
                message = ws.receive()
                if message.get("text") is not None:
                    assert json.loads(message["text"])["id"] == request_id
                    break
