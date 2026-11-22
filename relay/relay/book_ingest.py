"""Deterministic EPUB/TXT ingestion for the AI-native reading library.

The passage rules are the contract in docs/ai-native/api-v1.md:
- p/li counters are per spine document, 0-based, every element counts;
- text is visible text with nested p/li excluded (nested blocks own their text);
- passage_id = <book_id>:<spine>:<p|li><block_index>;
- char offsets are UTF-8 bytes within the normalized passage text.

EPUB container, spine and TOC parsing is delegated to ebooklib; chapter HTML
is parsed with BeautifulSoup (lxml backend), and TXT encoding detection uses
charset-normalizer. The passage mapping on top is ours because no library
exposes stable paragraph IDs. CSS layout and display:none are intentionally
ignored: V1 indexes visible block text, not rendered pages.
"""

from __future__ import annotations

import logging
import posixpath
import re
import unicodedata
import urllib.parse
import warnings
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING

from bs4 import BeautifulSoup, Comment, NavigableString, Tag, XMLParsedAsHTMLWarning
from charset_normalizer import from_bytes
from ebooklib import epub

if TYPE_CHECKING:
    from .library_store import LibraryStore

log = logging.getLogger("relay.book_ingest")

_SPINE_MEDIA = ("application/xhtml+xml", "text/html")
_XHTML_EXT = (".xhtml", ".html", ".htm")
_BLOCK_TAGS = ("p", "li")
_SKIP_TAGS = ("script", "style", "head")
try:
    import lxml  # noqa: F401

    _HTML_PARSER = "lxml"
except ImportError:  # pragma: no cover - lxml is a declared dependency
    _HTML_PARSER = "html.parser"
_CJK = "\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff"
_WS_RE = re.compile(r"\s+")
_CJK_GAP_RE = re.compile(rf"(?<=[{_CJK}])\s+(?=[{_CJK}])")
_CJK_PUNCT_GAP_RE = re.compile(rf"(?<=[{_CJK}])\s+(?=[，。！？；：、）》」』】])")
_PUNCT_CJK_GAP_RE = re.compile(rf"(?<=[（《「『【])\s+(?=[{_CJK}])")
_ZERO_WIDTH = dict.fromkeys(map(ord, "\u200b\u200c\u200d\ufeff"), None)
# Defensive ceiling for pathological uploads (a 64 MB log file of one-line
# paragraphs would otherwise build millions of dicts and exhaust RAM). A
# complete multi-volume Chinese work is in the tens of thousands of blocks.
MAX_BOOK_PASSAGES = 200_000


class IngestError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


@dataclass
class ParsedChapter:
    spine_index: int
    title: str
    href: str
    ordinal: int
    passages: list[dict] = field(default_factory=list)


@dataclass
class ParsedBook:
    title: str
    author: str
    language: str
    chapters: list[ParsedChapter]

    @property
    def passages(self) -> list[dict]:
        return [p for c in self.chapters for p in c.passages]

    @property
    def passage_count(self) -> int:
        return sum(len(c.passages) for c in self.chapters)


def normalize_text(text: str) -> str:
    """The single normalization used by both indexing and citation checks."""
    text = unicodedata.normalize("NFC", text).translate(_ZERO_WIDTH)
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = _WS_RE.sub(" ", text)
    text = _CJK_GAP_RE.sub("", text)
    text = _CJK_PUNCT_GAP_RE.sub("", text)
    text = _PUNCT_CJK_GAP_RE.sub("", text)
    return text.strip()


def normalize_title(text: str) -> str:
    """Titles keep intentional spaces (e.g. '第一章 山间')."""
    text = unicodedata.normalize("NFC", text).translate(_ZERO_WIDTH)
    return _WS_RE.sub(" ", text).strip()


def _decode_html(data: bytes) -> str:
    """UTF-8 first; charset-normalizer for legacy CJK EPUB/TXT encodings."""
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        pass
    best = from_bytes(data).best()
    if best is not None:
        return str(best)
    return data.decode("utf-8", errors="replace")


def _block_text(tag: Tag) -> str:
    """Visible text of one block, excluding nested p/li and script/style."""
    for br in tag.find_all("br"):
        br.replace_with(" ")
    parts: list[str] = []
    for node in tag.descendants:
        if not isinstance(node, NavigableString) or isinstance(node, Comment):
            continue
        parent = node.parent
        skip = False
        while parent is not None and parent is not tag:
            if isinstance(parent, Tag) and (parent.name in _BLOCK_TAGS or parent.name in _SKIP_TAGS):
                skip = True
                break
            parent = parent.parent
        if not skip:
            parts.append(str(node))
    return "".join(parts)


def _is_hidden(tag: Tag, root: Tag) -> bool:
    node: Tag | None = tag
    while node is not None and node is not root:
        if node.has_attr("hidden"):
            return True
        if str(node.get("aria-hidden", "")).lower() == "true":
            return True
        parent = node.parent
        node = parent if isinstance(parent, Tag) else None
    return False


def _chapter_passages(
    data: bytes,
    book_id: str,
    spine_index: int,
    *,
    max_passages: int = MAX_BOOK_PASSAGES,
) -> list[dict]:
    text = _decode_html(data)
    # XHTML is XML, but real EPUBs are often not well-formed. The HTML parser
    # is deliberately more forgiving; the warning would only add noise.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", XMLParsedAsHTMLWarning)
        soup = BeautifulSoup(text, _HTML_PARSER)
    root = soup.body or soup
    counters = {"p": 0, "l": 0}
    passages: list[dict] = []
    for tag in root.find_all(list(_BLOCK_TAGS)):
        if len(passages) >= max_passages:
            raise IngestError(
                "TOO_MANY_PASSAGES",
                f"book exceeds the {MAX_BOOK_PASSAGES} indexable paragraph limit",
            )
        kind = "p" if tag.name == "p" else "l"
        index = counters[kind]
        counters[kind] += 1
        if _is_hidden(tag, root):
            continue
        text = normalize_text(_block_text(tag))
        if not text:
            continue
        seq = len(passages)
        passages.append(
            {
                "passage_id": f"{book_id}:{spine_index}:{kind}{index}",
                "spine_index": spine_index,
                "seq": seq,
                "kind": kind,
                "block_index": index,
                "text": text,
                "char_start": 0,
                "char_end": len(text.encode("utf-8")),
            }
        )
    return passages


def _href_key(href: str) -> str:
    href = urllib.parse.unquote(href.split("#", 1)[0]).replace("\\", "/")
    return posixpath.normpath(href).lstrip("./")


def _toc_titles(book: epub.EpubBook) -> dict[str, str]:
    """Flatten ebooklib's nested TOC into href -> title."""
    titles: dict[str, str] = {}

    def add(href: str, title: str) -> None:
        title = normalize_title(title or "")
        key = _href_key(href or "")
        if key and title:
            titles.setdefault(key, title)

    def walk(entries) -> None:
        for entry in entries or []:
            if isinstance(entry, tuple):
                section, children = entry
                add(getattr(section, "href", ""), getattr(section, "title", ""))
                walk(children)
            else:
                add(getattr(entry, "href", ""), getattr(entry, "title", ""))

    walk(book.toc)
    return titles


def _title_for(href: str, titles: dict[str, str]) -> str:
    key = _href_key(href)
    if key in titles:
        return titles[key]
    # TOC hrefs may be relative to the nav/NCX file rather than the OPF root.
    for toc_href, title in titles.items():
        if key.endswith("/" + toc_href) or toc_href.endswith("/" + key):
            return title
    base = posixpath.basename(key)
    matches = [title for toc_href, title in titles.items() if posixpath.basename(toc_href) == base]
    return matches[0] if len(matches) == 1 else ""


def _meta_first(book: epub.EpubBook, name: str) -> str:
    values = book.get_metadata("DC", name)
    if not values:
        return ""
    value = values[0][0]
    return normalize_title(str(value)) if value is not None else ""


def _parse_epub(path: Path, book_id: str) -> ParsedBook:
    try:
        book = epub.read_epub(str(path), options={"ignore_ncx": False})
    except Exception as exc:  # ebooklib raises EpubException / zip errors
        raise IngestError("EPUB_PARSE_FAILED", f"cannot read EPUB: {exc}") from exc

    titles = _toc_titles(book)
    chapters: list[ParsedChapter] = []
    remaining = MAX_BOOK_PASSAGES
    for spine_index, entry in enumerate(book.spine or []):
        idref = entry[0] if isinstance(entry, (tuple, list)) and entry else entry
        item = book.get_item_with_id(idref) if idref else None
        if item is None:
            log.warning("spine item missing in manifest", extra={"spine": spine_index, "idref": idref})
            continue
        href = item.get_name() or ""
        media = (item.media_type or "").lower()
        if media not in _SPINE_MEDIA and not href.lower().endswith(_XHTML_EXT):
            continue
        try:
            data = item.get_content()
        except Exception as exc:  # corrupt zip member
            log.warning("spine item unreadable", extra={"spine": spine_index, "href": href, "error": str(exc)})
            continue
        passages = _chapter_passages(data, book_id, spine_index, max_passages=remaining)
        remaining -= len(passages)
        chapters.append(
            ParsedChapter(
                spine_index=spine_index,
                title=_title_for(href, titles) or f"Section {spine_index + 1}",
                href=href,
                ordinal=len(chapters),
                passages=passages,
            )
        )
    return ParsedBook(
        title=_meta_first(book, "title"),
        author=_meta_first(book, "creator"),
        language=_meta_first(book, "language"),
        chapters=chapters,
    )


def _parse_txt(path: Path, book_id: str) -> ParsedBook:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise IngestError("READ_FAILED", str(exc)) from exc
    text = _decode_html(raw)
    # Each non-empty line is one p block. This is deterministic even when the
    # TXT has no blank-line paragraph structure and matches the reader's
    # line-oriented progress model closely enough for V1 citations.
    lines = [normalize_text(line) for line in text.splitlines()]
    passages = []
    for line in lines:
        if not line:
            continue
        if len(passages) >= MAX_BOOK_PASSAGES:
            raise IngestError(
                "TOO_MANY_PASSAGES",
                f"book exceeds the {MAX_BOOK_PASSAGES} indexable paragraph limit",
            )
        index = len(passages)
        passages.append(
            {
                "passage_id": f"{book_id}:0:p{index}",
                "spine_index": 0,
                "seq": index,
                "kind": "p",
                "block_index": index,
                "text": line,
                "char_start": 0,
                "char_end": len(line.encode("utf-8")),
            }
        )
    chapter = ParsedChapter(
        spine_index=0,
        title=path.stem or "全文",
        href=path.name,
        ordinal=0,
        passages=passages,
    )
    return ParsedBook(title=path.stem or path.name, author="", language="", chapters=[chapter])


def parse_book(path: Path, book_id: str) -> ParsedBook:
    suffix = path.suffix.lower()
    if suffix == ".epub":
        parsed = _parse_epub(path, book_id)
    elif suffix == ".txt":
        parsed = _parse_txt(path, book_id)
    else:
        raise IngestError("UNSUPPORTED_FORMAT", f"unsupported format {suffix!r}")
    if parsed.passage_count == 0:
        raise IngestError("EMPTY_BOOK", "no indexable p/li passages")
    return parsed


def _mark_failed(store: "LibraryStore", book_id: str, code: str, message: str) -> None:
    """Best-effort terminal state; the DB itself may be the failing component."""
    try:
        store.update_book_status(
            book_id,
            status="failed",
            progress=0.0,
            error_code=code,
            error_message=message[:500],
        )
    except Exception:
        log.exception("could not mark book failed", extra={"book_id": book_id, "code": code})


def ingest_book(store: "LibraryStore", book_id: str, path: Path) -> None:
    """Parse a stored file into the library. Safe to run in a worker thread."""
    try:
        store.update_book_status(book_id, status="parsing", progress=0.05)
        parsed = parse_book(path, book_id)
        chapters = [
            {
                "spine_index": c.spine_index,
                "title": c.title,
                "href": c.href,
                "ordinal": c.ordinal,
                "passage_count": len(c.passages),
            }
            for c in parsed.chapters
        ]
        store.update_book_metadata(
            book_id,
            title=parsed.title or f"Section {book_id}",
            author=parsed.author,
            language=parsed.language,
        )
        store.update_book_status(
            book_id,
            status="indexing",
            progress=0.9,
            chapters=len(chapters),
            passages=parsed.passage_count,
        )
        store.replace_book_content(book_id, chapters, parsed.passages)
        store.update_book_status(
            book_id,
            status="ready",
            progress=1.0,
            chapters=len(chapters),
            passages=parsed.passage_count,
        )
    except IngestError as exc:
        _mark_failed(store, book_id, exc.code, exc.message)
        log.warning("book ingest failed", extra={"book_id": book_id, "code": exc.code, "error": exc.message})
        return
    except Exception as exc:  # parse crash, disk full, DB error: never stay "indexing"
        _mark_failed(store, book_id, "INTERNAL_ERROR", str(exc))
        log.exception("book ingest crashed", extra={"book_id": book_id})
        return

    log.info(
        "book ingest ready",
        extra={"book_id": book_id, "chapters": len(chapters), "passages": parsed.passage_count},
    )
