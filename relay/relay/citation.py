"""Parse and validate model answers for the reader query endpoint.

The model is allowed to suggest citations; it is never trusted to label them.
Every citation is checked against the exact passages that were sent as context.
A ``book`` segment without a surviving citation is downgraded to ``inference``.
"""

from __future__ import annotations

import json
import logging
from typing import Any

from .book_ingest import normalize_text

log = logging.getLogger("relay.citation")

SOURCES = ("book", "external", "inference")
MAX_SEGMENTS = 12
MAX_CITATIONS_PER_SEGMENT = 8
MAX_SEGMENT_CHARS = 3000
MAX_TOTAL_CHARS = 8000

_SOURCE_ALIASES = {
    "book": "book",
    "source": "book",
    "quote": "book",
    "原文": "book",
    "书中原文": "book",
    "external": "external",
    "外部": "external",
    "外部补充": "external",
    "inference": "inference",
    "推断": "inference",
}

_WARNING_ORDER = (
    "MODEL_OUTPUT_NOT_JSON",
    "CITATION_DROPPED",
    "SEGMENT_TRUNCATED",
    "SEGMENTS_TRUNCATED",
)


def _warn(warnings: list[str], code: str) -> None:
    if code not in warnings:
        warnings.append(code)


def _strip_fence(text: str) -> str:
    text = text.strip()
    if text.startswith("```"):
        lines = text.splitlines()
        if lines and lines[0].startswith("```"):
            lines = lines[1:]
        if lines and lines[-1].strip() == "```":
            lines = lines[:-1]
        text = "\n".join(lines).strip()
    return text


def _load_json(text: str) -> Any:
    text = _strip_fence(text)
    try:
        return json.loads(text)
    except (TypeError, ValueError):
        pass
    # Tolerate a short prose preamble/epilogue around the JSON object.
    for opener, closer in (("{", "}"), ("[", "]")):
        start = text.find(opener)
        end = text.rfind(closer)
        if start >= 0 and end > start:
            try:
                return json.loads(text[start : end + 1])
            except (TypeError, ValueError):
                continue
    return None


def _truncate(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    # Keep complete UTF-8 sequences; JSON text is already a Python str.
    return text[:limit].rstrip()


def _coerce_citation(raw: Any, allowed: dict[str, dict]) -> dict | None:
    if not isinstance(raw, dict):
        return None
    passage_id = str(raw.get("passage_id") or raw.get("passageId") or "").strip()
    passage = allowed.get(passage_id)
    if passage is None:
        return None
    quote = normalize_text(str(raw.get("quote") or ""))
    if not quote:
        return None
    if quote not in normalize_text(passage["text"]):
        return None
    return {
        "passage_id": passage_id,
        "spine_index": int(passage["spine_index"]),
        "quote": quote,
    }


def validate_segments(
    raw_segments: Any,
    *,
    allowed: dict[str, dict],
    current_spine: int | None = None,
    chapter_scope: bool = False,
) -> tuple[list[dict], list[str]]:
    """Coerce model output into the public schema, enforcing citations.

    ``allowed`` is the exact passage set sent to the model, keyed by passage_id.
    """
    warnings: list[str] = []
    if isinstance(raw_segments, dict):
        if "segments" in raw_segments:
            raw_segments = raw_segments["segments"]
        elif "answer" in raw_segments:
            raw_segments = [{"source": "inference", "text": raw_segments.get("answer")}]
        else:
            raw_segments = [raw_segments]
    if not isinstance(raw_segments, list):
        raw_segments = []

    out: list[dict] = []
    total = 0
    for item in raw_segments:
        if not isinstance(item, dict):
            continue
        if len(out) >= MAX_SEGMENTS:
            _warn(warnings, "SEGMENTS_TRUNCATED")
            break
        text = _truncate(normalize_text(str(item.get("text") or item.get("content") or "")), MAX_SEGMENT_CHARS)
        if not text:
            continue
        if total + len(text) > MAX_TOTAL_CHARS:
            text = _truncate(text, max(0, MAX_TOTAL_CHARS - total))
            _warn(warnings, "SEGMENT_TRUNCATED")
            if not text:
                break
        source = _SOURCE_ALIASES.get(str(item.get("source") or "").strip().lower())
        if source is None:
            source = "inference"
        citations: list[dict] = []
        if source in ("book", "external"):
            raw_citations = item.get("citations") or item.get("citation") or []
            if isinstance(raw_citations, dict):
                raw_citations = [raw_citations]
            if isinstance(raw_citations, list):
                for raw in raw_citations[:MAX_CITATIONS_PER_SEGMENT]:
                    citation = _coerce_citation(raw, allowed)
                    if citation is None:
                        _warn(warnings, "CITATION_DROPPED")
                        continue
                    if chapter_scope and current_spine is not None and citation["spine_index"] != current_spine:
                        _warn(warnings, "CITATION_DROPPED")
                        continue
                    citations.append(citation)
            if source == "book" and not citations:
                source = "inference"
                _warn(warnings, "CITATION_DROPPED")
            if source == "external":
                citations = []  # external claims are not book-grounded
        out.append({"source": source, "text": text, "citations": citations})
        total += len(text)
    if not out:
        _warn(warnings, "MODEL_OUTPUT_NOT_JSON")
    # Stable warning order for the device log.
    warnings.sort(key=lambda w: _WARNING_ORDER.index(w) if w in _WARNING_ORDER else len(_WARNING_ORDER))
    return out, warnings


def parse_model_output(
    text: str,
    *,
    allowed: dict[str, dict],
    current_spine: int | None = None,
    chapter_scope: bool = False,
) -> tuple[list[dict], list[str]]:
    payload = _load_json(text or "")
    warnings: list[str] = []
    if payload is None:
        fallback = _truncate(normalize_text(text or ""), MAX_TOTAL_CHARS)
        if not fallback:
            return [], ["MODEL_OUTPUT_NOT_JSON"]
        return [{"source": "inference", "text": fallback, "citations": []}], ["MODEL_OUTPUT_NOT_JSON"]
    segments, warnings = validate_segments(
        payload,
        allowed=allowed,
        current_spine=current_spine,
        chapter_scope=chapter_scope,
    )
    if not segments:
        _warn(warnings, "MODEL_OUTPUT_NOT_JSON")
    return segments, warnings
