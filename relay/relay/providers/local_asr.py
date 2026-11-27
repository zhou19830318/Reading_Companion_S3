from __future__ import annotations

import httpx

from ..config import Settings
from ..protocol import ErrorCode
from .base import AsrContract, AsrProvider, AsrResult, ProviderError

contract = AsrContract(
    name="local-sensevoice-onnx-int8",
    endpoint="http://127.0.0.1:18100/v1/asr",
    auth="local loopback",
    region="local",
    model="SenseVoiceSmall-onnx-int8",
    input_format="pcm_s16le, 16000 Hz, mono",
    chunk_ms=0,
    events={
        "audio.end": "POST buffered PCM",
        "asr.final": "local transcription",
    },
    heartbeat="none",
    reconnect="session retry",
)


class LocalSenseVoiceProvider(AsrProvider):
    contract = contract

    def __init__(self, settings: Settings):
        super().__init__()
        self.settings = settings
        self._buffer = bytearray()
        self._client: httpx.AsyncClient | None = None

    async def start(self) -> None:
        self._client = httpx.AsyncClient(
            timeout=httpx.Timeout(
                connect=2,
                read=self.settings.asr_finish_timeout,
                write=5,
                pool=2,
            )
        )

    async def push_audio(self, pcm: bytes) -> None:
        if len(self._buffer) + len(pcm) > self.settings.max_audio_seconds * 32000:
            raise ProviderError(ErrorCode.AUDIO_TIMEOUT, "local asr buffer exceeded")
        self._buffer.extend(pcm)

    async def finish(self) -> None:
        if not self._buffer:
            raise ProviderError(ErrorCode.ASR_NO_SPEECH, "empty local asr buffer")
        assert self._client is not None
        try:
            response = await self._client.post(
                self.settings.asr_local_url,
                files={"file": ("audio.pcm", bytes(self._buffer), "application/octet-stream")},
                data={"sample_rate": "16000", "format": "pcm_s16le"},
            )
            if response.status_code >= 500:
                raise ProviderError(ErrorCode.ASR_CONNECTION_ERROR, f"local asr http {response.status_code}")
            if response.status_code >= 400:
                raise ProviderError(ErrorCode.INTERNAL_ERROR, f"local asr http {response.status_code}")
            text = str(response.json().get("text") or "").strip()
            if not text:
                raise ProviderError(ErrorCode.ASR_NO_SPEECH, "local asr empty")
            await self._emit(AsrResult(text=text, is_final=True, end_ms=len(self._buffer) // 32))
        except ProviderError:
            raise
        except Exception as exc:
            raise ProviderError(ErrorCode.ASR_CONNECTION_ERROR, f"local asr request: {exc}") from exc

    async def close(self) -> None:
        if self._client is not None:
            await self._client.aclose()
        self._client = None
        self._buffer.clear()
