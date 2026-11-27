from __future__ import annotations

import httpx
import pytest

from relay.config import Settings
from relay.protocol import ErrorCode
from relay.providers.base import ProviderError
from relay.providers.local_asr import LocalSenseVoiceProvider


def local_settings(**env) -> Settings:
    values = {"ASR_PROVIDER": "local-sensevoice", "LLM_PROVIDER": "mock", **env}
    return Settings(**values)


@pytest.mark.parametrize("name", ["local", "sensevoice", "local-sensevoice", "local_sensevoice"])
def test_local_sensevoice_names_are_normalized_and_ready(name: str):
    settings = local_settings(ASR_PROVIDER=name)
    assert settings.asr_provider == "local_sensevoice"
    assert settings._asr_ready()


def test_local_sensevoice_requires_an_endpoint():
    assert not local_settings(ASR_LOCAL_URL="")._asr_ready()


@pytest.mark.asyncio
async def test_buffered_pcm_is_posted_and_final_text_is_emitted():
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["url"] = str(request.url)
        seen["form"] = request.content
        return httpx.Response(200, json={"text": "  今天天气很好  "})

    provider = LocalSenseVoiceProvider(local_settings())
    provider._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    results = []

    async def record(result):
        results.append(result)

    provider.on_result(record)
    await provider.push_audio(b"\x01\x00" * 3200)
    await provider.finish()
    await provider.close()

    assert seen["url"] == "http://127.0.0.1:18100/v1/asr"
    assert b'name="sample_rate"\r\n\r\n16000' in seen["form"]
    assert b'name="format"\r\n\r\npcm_s16le' in seen["form"]
    assert len(results) == 1
    assert results[0].text == "今天天气很好"
    assert results[0].is_final is True
    assert results[0].end_ms == 200


@pytest.mark.asyncio
async def test_empty_local_transcription_is_reported():
    provider = LocalSenseVoiceProvider(local_settings())
    provider._client = httpx.AsyncClient(
        transport=httpx.MockTransport(lambda request: httpx.Response(200, json={"text": ""}))
    )
    await provider.push_audio(b"\x01\x00" * 3200)

    with pytest.raises(ProviderError) as excinfo:
        await provider.finish()
    await provider.close()

    assert excinfo.value.code == ErrorCode.ASR_NO_SPEECH
