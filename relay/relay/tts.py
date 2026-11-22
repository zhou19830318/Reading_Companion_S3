"""edge-tts to 16 kHz mono PCM, streamed as WebSocket binary frames.

The firmware has no audio decoder (``HalAudio::writePcm`` takes int16 PCM at a
fixed 16 kHz), and edge-tts can only emit MP3 — its ``outputFormat`` is pinned
to ``audio-24khz-48kbitrate-mono-mp3`` upstream. So the relay does the decoding
and resampling and hands the device raw samples. ``miniaudio`` ships as a pip
wheel with a bundled decoder, which keeps this a Python dependency rather than a
system package.

Synthesis is segmented: a whole page would be minutes of audio and the device
only buffers two seconds, so the text is cut into short segments that are
synthesised ahead of playback and pipelined. That way a ~1 s synthesis cost
never becomes a gap in the audio; the queue's backpressure paces production to
the device's consumption.

Both third-party imports are lazy: a deployment that does not want TTS can leave
``edge-tts``/``miniaudio`` uninstalled and still boot — ``available()`` reports
why it is off instead of the process failing at import time.
"""

from __future__ import annotations

import asyncio
import logging
from collections.abc import AsyncIterator

from .config import Settings

log = logging.getLogger("relay.tts")

SAMPLE_RATE = 16000
FRAME_BYTES = 8192  # one WebSocket binary message = 256 ms at 16 kHz mono
# Characters per synthesis segment. 120 CJK characters is ~30 s of audio, so
# segment boundaries are rarer and edge-tts has more context for natural
# prosody; a ~1 s synthesis round still hides behind the previous segment and
# the two-deep pipeline stays under a couple of megabytes per connection.
SEGMENT_CHARS = 120
PIPELINE_DEPTH = 2
SEGMENT_BREAKS = "。！？!?；;\n\r"
SOFT_BREAKS = "，、,：:）)】》"


class TtsError(Exception):
    def __init__(self, code: str, status: int = 502):
        super().__init__(code)
        self.code, self.status = code, status


def _load_modules() -> tuple[object, object] | None:
    try:
        import edge_tts  # noqa: F401
        import miniaudio  # noqa: F401
    except ImportError:
        return None
    return edge_tts, miniaudio


def available() -> bool:
    return _load_modules() is not None


def voice_for(settings: Settings, lang: str) -> str:
    """Chinese unless the caller asked for English; unknown languages read Chinese.

    The device sends the card's language explicitly (a Hanzi card is zh, a word
    card is en) rather than letting the relay guess from the text: an English
    word inside a Chinese page must still be spoken by the English voice.
    """
    return settings.tts_voice_en if str(lang).lower().startswith("en") else settings.tts_voice_zh


def normalize_text(text: str) -> str:
    """Collapse whitespace and drop control characters.

    Layout text arrives with hard line breaks from the reader's page model;
    feeding those to the synthesiser produces unnatural long pauses, so a line
    break becomes a space unless it already follows sentence punctuation.
    """
    out: list[str] = []
    for ch in text:
        if ch in "\r\t":
            out.append(" ")
        elif ch == "\n":
            previous = out[-1] if out else ""
            out.append(" " if previous and previous not in SEGMENT_BREAKS else "")
        elif ch.isprintable() or ch == "\n":
            out.append(ch)
    return " ".join("".join(out).split()).strip()


def segment_text(text: str, limit: int = SEGMENT_CHARS) -> list[str]:
    """Split into synthesis segments at sentence, then clause, then character.

    Preferring a sentence break keeps prosody natural; the soft break exists so
    a long Chinese sentence (no spaces, punctuation only at the end) still cuts
    near a comma instead of mid-phrase.
    """
    if limit < 8:
        limit = 8
    segments: list[str] = []
    rest = text.strip()
    while rest:
        if len(rest) <= limit:
            segments.append(rest)
            break
        window = rest[:limit]
        cut = -1
        for index in range(len(window) - 1, 0, -1):
            if window[index] in SEGMENT_BREAKS:
                cut = index + 1
                break
        if cut <= 0:
            for index in range(len(window) - 1, 0, -1):
                if window[index] in SOFT_BREAKS:
                    cut = index + 1
                    break
        if cut <= 0:
            cut = limit
        segments.append(rest[:cut].strip())
        rest = rest[cut:].strip()
    return [segment for segment in segments if segment]


async def _synthesize_mp3(edge_tts: object, text: str, voice: str, timeout: float) -> bytes:
    """One segment of MP3. Raises TtsError on an upstream refusal or timeout."""

    async def collect() -> bytes:
        communicate = edge_tts.Communicate(text, voice)  # type: ignore[attr-defined]
        buffer = bytearray()
        async for chunk in communicate.stream():
            if chunk.get("type") == "audio":
                buffer.extend(chunk["data"])
                if len(buffer) > 8 * 1024 * 1024:
                    raise TtsError("upstream_too_large")
        return bytes(buffer)

    try:
        mp3 = await asyncio.wait_for(collect(), timeout=timeout)
    except asyncio.TimeoutError as exc:
        raise TtsError("upstream_timeout", 504) from exc
    except TtsError:
        raise
    except Exception as exc:  # edge-tts raises aiohttp/websocket errors of many types
        raise TtsError("upstream_failed") from exc
    if not mp3:
        raise TtsError("upstream_empty")
    return mp3


def _decode_pcm(miniaudio: object, mp3: bytes) -> bytes:
    """MP3 -> 16 kHz mono signed 16-bit little-endian PCM."""
    try:
        decoded = miniaudio.decode(  # type: ignore[attr-defined]
            mp3,
            output_format=miniaudio.SampleFormat.SIGNED16,  # type: ignore[attr-defined]
            nchannels=1,
            sample_rate=SAMPLE_RATE,
        )
    except Exception as exc:
        raise TtsError("decode_failed") from exc
    return decoded.samples.tobytes()


def prepare(text: str, settings: Settings) -> tuple[str, bool]:
    """Normalise and cap the text. Returns (speakable text, was_truncated)."""
    cleaned = normalize_text(text)
    truncated = len(cleaned) > settings.tts_max_chars
    return (cleaned[: settings.tts_max_chars] if truncated else cleaned), truncated


async def pcm_frames(text: str, voice: str, settings: Settings) -> AsyncIterator[bytes]:
    """Yield PCM frames for ``text``, synthesising ahead of playback.

    The producer owns the timing: it fills a small queue of decoded segments and
    blocks once the device is behind, so memory stays bounded per connection
    while segment boundaries stay inaudible.
    """
    modules = _load_modules()
    if modules is None:
        raise TtsError("unavailable", 503)
    edge_tts, miniaudio = modules

    if not text.strip():
        raise TtsError("bad_text", 400)

    segments = segment_text(text, settings.tts_segment_chars)
    # Items are decoded PCM, a producer failure to re-raise, or None for "done".
    # The failure travels through the queue because the producer runs as a task:
    # a task's exception would otherwise be swallowed and the route would report
    # a clean tts.end after half a page.
    queue: asyncio.Queue[object] = asyncio.Queue(maxsize=PIPELINE_DEPTH)

    async def put(item: object) -> None:
        try:
            await queue.put(item)
        except asyncio.CancelledError:
            raise
        except Exception:  # consumer gone; nothing left to report to
            pass

    async def produce() -> None:
        try:
            for segment in segments:
                mp3 = await _synthesize_mp3(edge_tts, segment, voice, settings.tts_timeout_s)
                pcm = await asyncio.to_thread(_decode_pcm, miniaudio, mp3)
                await put(pcm)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            await put(exc)
        await put(None)

    producer = asyncio.create_task(produce())
    try:
        while True:
            block = await queue.get()
            if block is None:
                break
            if isinstance(block, BaseException):
                raise block
            for offset in range(0, len(block), FRAME_BYTES):  # type: ignore[arg-type]
                yield block[offset : offset + FRAME_BYTES]
    finally:
        if not producer.done():
            producer.cancel()
        await asyncio.gather(producer, return_exceptions=True)
