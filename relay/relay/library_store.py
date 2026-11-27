"""SQLite-backed library, annotations and sync operations for AI-native reading.

Design mirrors learning_store.py / pomodoro_store.py:
- One connection per process, opened in the FastAPI lifespan. Statements are
  ms-scale; the process is the only writer.
- Idempotent schema creation, WAL, busy timeout.
- Passage IDs are derived from the source bytes, never from page layout:
  ``<book_id>:<spine>:<p|li><block_index>`` where book_id is the first 16 hex
  characters of the file SHA-256. See docs/ai-native/api-v1.md.
- Sync idempotency is a UNIQUE ``sync_ops.op_id``: a retry returns the stored
  result instead of applying the operation twice.
"""

from __future__ import annotations

import json
import re
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 2

BOOK_STATUSES = ("pending", "parsing", "indexing", "ready", "failed")
SYNC_TYPES = (
    "annotation.create",
    "annotation.delete",
    "note.create",
    "note.delete",
    "ai_result.create",
)

_ANNOTATION_KINDS = ("highlight", "note", "voice_note", "ai_saved")


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def _json_loads(value: str, default: Any) -> Any:
    try:
        return json.loads(value)
    except (TypeError, ValueError):
        return default


def _json_dumps(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


_SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS books (
    id            TEXT PRIMARY KEY,
    sha256        TEXT NOT NULL UNIQUE,
    title         TEXT NOT NULL DEFAULT '',
    author        TEXT NOT NULL DEFAULT '',
    language      TEXT NOT NULL DEFAULT '',
    format        TEXT NOT NULL DEFAULT '',
    size          INTEGER NOT NULL DEFAULT 0,
    file_path     TEXT NOT NULL DEFAULT '',
    status        TEXT NOT NULL DEFAULT 'pending',
    progress      REAL NOT NULL DEFAULT 0,
    chapters      INTEGER NOT NULL DEFAULT 0,
    passages      INTEGER NOT NULL DEFAULT 0,
    error_code    TEXT NOT NULL DEFAULT '',
    error_message TEXT NOT NULL DEFAULT '',
    deleted       INTEGER NOT NULL DEFAULT 0,
    created_at    TEXT NOT NULL,
    updated_at    TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_books_status ON books (status, updated_at);

CREATE TABLE IF NOT EXISTS chapters (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    book_id       TEXT NOT NULL,
    spine_index   INTEGER NOT NULL,
    title         TEXT NOT NULL DEFAULT '',
    href          TEXT NOT NULL DEFAULT '',
    ordinal       INTEGER NOT NULL,
    passage_count INTEGER NOT NULL DEFAULT 0,
    UNIQUE (book_id, spine_index)
);
CREATE INDEX IF NOT EXISTS idx_chapters_book ON chapters (book_id, ordinal);

CREATE TABLE IF NOT EXISTS passages (
    id          TEXT PRIMARY KEY,
    book_id     TEXT NOT NULL,
    chapter_id  INTEGER NOT NULL,
    spine_index INTEGER NOT NULL,
    seq         INTEGER NOT NULL,
    block_kind  TEXT NOT NULL,
    block_index INTEGER NOT NULL,
    text        TEXT NOT NULL,
    char_start  INTEGER NOT NULL DEFAULT 0,
    char_end    INTEGER NOT NULL DEFAULT 0,
    UNIQUE (book_id, spine_index, block_kind, block_index),
    UNIQUE (book_id, spine_index, seq)
);
CREATE INDEX IF NOT EXISTS idx_passages_book_spine ON passages (book_id, spine_index, seq);

CREATE VIRTUAL TABLE IF NOT EXISTS passages_fts USING fts5(
    text, content='passages', content_rowid='rowid', tokenize='trigram'
);

CREATE TABLE IF NOT EXISTS annotations (
    id          TEXT PRIMARY KEY,
    device_id   TEXT NOT NULL,
    book_id     TEXT NOT NULL,
    passage_id  TEXT NOT NULL DEFAULT '',
    quote       TEXT NOT NULL DEFAULT '',
    note        TEXT NOT NULL DEFAULT '',
    kind        TEXT NOT NULL DEFAULT 'highlight',
    anchor_json TEXT NOT NULL DEFAULT '{}',
    deleted     INTEGER NOT NULL DEFAULT 0,
    created_at  TEXT NOT NULL,
    updated_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_annotations_device ON annotations (device_id, updated_at);
CREATE INDEX IF NOT EXISTS idx_annotations_book ON annotations (book_id, updated_at);

CREATE TABLE IF NOT EXISTS ai_results (
    id            TEXT PRIMARY KEY,
    device_id     TEXT NOT NULL,
    book_id       TEXT NOT NULL,
    passage_ids   TEXT NOT NULL DEFAULT '[]',
    question      TEXT NOT NULL DEFAULT '',
    segments_json TEXT NOT NULL DEFAULT '[]',
    deleted       INTEGER NOT NULL DEFAULT 0,
    created_at    TEXT NOT NULL,
    updated_at    TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_ai_results_device ON ai_results (device_id, updated_at);

CREATE TABLE IF NOT EXISTS summaries (
    cache_key       TEXT PRIMARY KEY,
    book_id         TEXT NOT NULL,
    scope           TEXT NOT NULL,
    spine_index     INTEGER,
    content_revision TEXT NOT NULL,
    prompt_version  TEXT NOT NULL,
    model           TEXT NOT NULL,
    title           TEXT NOT NULL DEFAULT '',
    text            TEXT NOT NULL,
    generated_at    TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_summaries_book ON summaries (book_id, scope, spine_index);

CREATE TABLE IF NOT EXISTS sync_ops (
    op_id      TEXT PRIMARY KEY,
    device_id  TEXT NOT NULL,
    type       TEXT NOT NULL,
    entity_id  TEXT NOT NULL DEFAULT '',
    result     TEXT NOT NULL,
    error      TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS sync_changes (
    seq         INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    entity_type TEXT NOT NULL,
    entity_id   TEXT NOT NULL,
    op          TEXT NOT NULL,
    updated_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sync_changes_device ON sync_changes (device_id, seq);
"""


class LibraryStore:
    def __init__(self, db_path: Path, books_dir: Path) -> None:
        self.db_path = Path(db_path)
        self.books_dir = Path(books_dir)
        self._conn: sqlite3.Connection | None = None

    def open(self) -> None:
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        self.books_dir.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(self.db_path, timeout=5.0, check_same_thread=False)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA busy_timeout=5000")
        conn.execute("PRAGMA synchronous=NORMAL")
        conn.execute("PRAGMA foreign_keys=ON")
        conn.executescript(_SCHEMA)
        conn.execute("INSERT OR IGNORE INTO meta(key, value) VALUES('schema_version', ?)", (str(SCHEMA_VERSION),))
        conn.execute("UPDATE meta SET value=? WHERE key='schema_version'", (str(SCHEMA_VERSION),))
        conn.commit()
        self._conn = conn

    def close(self) -> None:
        if self._conn is not None:
            self._conn.close()
            self._conn = None

    @property
    def conn(self) -> sqlite3.Connection:
        if self._conn is None:
            raise RuntimeError("LibraryStore is not open (lifespan did not run?)")
        return self._conn

    def schema_version(self) -> int:
        row = self.conn.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
        return int(row["value"]) if row else 0

    # ---------------------------------------------------------------- books

    def create_book(
        self,
        *,
        sha256: str,
        title: str,
        author: str,
        language: str,
        book_format: str,
        size: int,
        file_path: str,
    ) -> dict:
        """Insert a new book or return the existing row for the same hash."""
        sha256 = sha256.lower()
        book_id = sha256[:16]
        existing = self.get_book_by_hash(sha256)
        if existing is not None:
            return existing
        collision = self.get_book(book_id)
        if collision is not None:
            # 64-bit prefixes collide with negligible probability in a personal
            # library; refuse rather than silently merging two different books.
            raise ValueError(f"book id collision: {book_id}")
        now = _now()
        self.conn.execute(
            "INSERT INTO books(id, sha256, title, author, language, format, size, file_path,"
            " status, progress, created_at, updated_at)"
            " VALUES(?,?,?,?,?,?,?,?,'pending',0,?,?)",
            (book_id, sha256, title, author, language, book_format, size, file_path, now, now),
        )
        self.conn.commit()
        return self.get_book(book_id)  # type: ignore[return-value]

    def get_book(self, book_id: str) -> dict | None:
        row = self.conn.execute("SELECT * FROM books WHERE id=? AND deleted=0", (book_id,)).fetchone()
        return self._book_row(row) if row else None

    def get_book_by_hash(self, sha256: str, *, include_deleted: bool = False) -> dict | None:
        sql = "SELECT * FROM books WHERE sha256=?"
        if not include_deleted:
            sql += " AND deleted=0"
        row = self.conn.execute(sql, (sha256.lower(),)).fetchone()
        return self._book_row(row) if row else None

    def list_books(self, limit: int = 100) -> list[dict]:
        rows = self.conn.execute(
            "SELECT * FROM books WHERE deleted=0 ORDER BY created_at DESC LIMIT ?", (max(1, limit),)
        ).fetchall()
        return [self._book_row(r) for r in rows]

    def set_book_file(self, book_id: str, path: Path) -> None:
        self.conn.execute(
            "UPDATE books SET file_path=?, updated_at=? WHERE id=?", (str(path), _now(), book_id)
        )
        self.conn.commit()

    def delete_book(self, book_id: str) -> bool:
        """Soft delete: passages and annotations stay for history/tombstones."""
        cur = self.conn.execute(
            "UPDATE books SET deleted=1, updated_at=? WHERE id=? AND deleted=0",
            (_now(), book_id),
        )
        self.conn.commit()
        return cur.rowcount > 0

    def restore_book(self, book_id: str, *, title: str = "", author: str = "") -> dict | None:
        """Undo a soft delete when the same bytes are uploaded again.

        Returns None when the row was not deleted, so the caller can keep the
        normal duplicate path. Status goes back to pending: a book deleted
        while parsing/failed must be re-indexed from the freshly uploaded file.
        Passages and annotations kept by the soft delete become visible again.
        """
        sets = [
            "deleted=0",
            "status='pending'",
            "progress=0",
            "error_code=''",
            "error_message=''",
            "updated_at=?",
        ]
        args: list[Any] = [_now()]
        if title:
            sets.append("title=?")
            args.append(title[:500])
        if author:
            sets.append("author=?")
            args.append(author[:500])
        args.append(book_id)
        cur = self.conn.execute(
            f"UPDATE books SET {', '.join(sets)} WHERE id=? AND deleted=1", args
        )
        self.conn.commit()
        return self.get_book(book_id) if cur.rowcount > 0 else None

    def update_book_metadata(self, book_id: str, *, title: str, author: str, language: str) -> None:
        self.conn.execute(
            "UPDATE books SET title=?, author=?, language=?, updated_at=? WHERE id=?",
            (title, author, language, _now(), book_id),
        )
        self.conn.commit()

    def update_book_status(
        self,
        book_id: str,
        *,
        status: str,
        progress: float | None = None,
        chapters: int | None = None,
        passages: int | None = None,
        error_code: str = "",
        error_message: str = "",
    ) -> None:
        if status not in BOOK_STATUSES:
            raise ValueError(f"unknown book status {status!r}")
        sets = ["status=?", "updated_at=?"]
        args: list[Any] = [status, _now()]
        if progress is not None:
            sets.append("progress=?")
            args.append(max(0.0, min(1.0, float(progress))))
        if chapters is not None:
            sets.append("chapters=?")
            args.append(int(chapters))
        if passages is not None:
            sets.append("passages=?")
            args.append(int(passages))
        sets.extend(["error_code=?", "error_message=?"])
        args.extend([error_code, error_message])
        args.append(book_id)
        self.conn.execute(f"UPDATE books SET {', '.join(sets)} WHERE id=?", args)
        self.conn.commit()

    def replace_book_content(self, book_id: str, chapters: list[dict], passages: list[dict]) -> None:
        """Replace parsed chapters/passages and rebuild the FTS rows atomically."""
        conn = self.conn
        with conn:
            conn.execute(
                "DELETE FROM passages_fts WHERE rowid IN (SELECT rowid FROM passages WHERE book_id=?)",
                (book_id,),
            )
            conn.execute("DELETE FROM passages WHERE book_id=?", (book_id,))
            conn.execute("DELETE FROM chapters WHERE book_id=?", (book_id,))
            chapter_ids: dict[int, int] = {}
            for ch in chapters:
                cur = conn.execute(
                    "INSERT INTO chapters(book_id, spine_index, title, href, ordinal, passage_count)"
                    " VALUES(?,?,?,?,?,?)",
                    (
                        book_id,
                        int(ch["spine_index"]),
                        ch.get("title", ""),
                        ch.get("href", ""),
                        int(ch["ordinal"]),
                        int(ch.get("passage_count", 0)),
                    ),
                )
                chapter_ids[int(ch["spine_index"])] = int(cur.lastrowid)
            for ps in passages:
                conn.execute(
                    "INSERT INTO passages(id, book_id, chapter_id, spine_index, seq, block_kind,"
                    " block_index, text, char_start, char_end) VALUES(?,?,?,?,?,?,?,?,?,?)",
                    (
                        ps["passage_id"],
                        book_id,
                        chapter_ids[int(ps["spine_index"])],
                        int(ps["spine_index"]),
                        int(ps["seq"]),
                        ps["kind"],
                        int(ps["block_index"]),
                        ps["text"],
                        int(ps.get("char_start", 0)),
                        int(ps.get("char_end", len(ps["text"].encode("utf-8")))),
                    ),
                )
            conn.execute(
                "INSERT INTO passages_fts(rowid, text) SELECT rowid, text FROM passages WHERE book_id=?",
                (book_id,),
            )
            conn.execute(
                "UPDATE books SET chapters=?, passages=?, updated_at=? WHERE id=?",
                (len(chapters), len(passages), _now(), book_id),
            )

    # -------------------------------------------------------------- queries

    def list_chapters(self, book_id: str) -> list[dict]:
        rows = self.conn.execute(
            "SELECT spine_index, title, ordinal, passage_count FROM chapters"
            " WHERE book_id=? ORDER BY ordinal",
            (book_id,),
        ).fetchall()
        return [
            {
                "spine_index": r["spine_index"],
                "title": r["title"],
                "ordinal": r["ordinal"],
                "passages": r["passage_count"],
            }
            for r in rows
        ]

    def chapter_title(self, book_id: str, spine_index: int) -> str:
        row = self.conn.execute(
            "SELECT title FROM chapters WHERE book_id=? AND spine_index=?", (book_id, spine_index)
        ).fetchone()
        return row["title"] if row else ""

    def list_passages(
        self,
        book_id: str,
        *,
        spine_index: int | None = None,
        limit: int = 200,
        after: str | None = None,
    ) -> list[dict]:
        sql = "SELECT * FROM passages WHERE book_id=?"
        args: list[Any] = [book_id]
        if spine_index is not None:
            sql += " AND spine_index=?"
            args.append(spine_index)
        if after:
            row = self.conn.execute("SELECT spine_index, seq FROM passages WHERE id=?", (after,)).fetchone()
            if row is not None:
                sql += " AND (spine_index > ? OR (spine_index = ? AND seq > ?))"
                args.extend([row["spine_index"], row["spine_index"], row["seq"]])
        sql += " ORDER BY spine_index, seq LIMIT ?"
        args.append(max(1, min(1000, limit)))
        rows = self.conn.execute(sql, args).fetchall()
        return [self._passage_row(r, book_id) for r in rows]

    def get_passages(self, book_id: str, passage_ids: list[str]) -> dict[str, dict]:
        if not passage_ids:
            return {}
        marks = ",".join("?" for _ in passage_ids)
        rows = self.conn.execute(
            f"SELECT * FROM passages WHERE book_id=? AND id IN ({marks})", [book_id, *passage_ids]
        ).fetchall()
        return {r["id"]: self._passage_row(r, book_id) for r in rows}

    def chapter_passages(self, book_id: str, spine_index: int) -> list[dict]:
        rows = self.conn.execute(
            "SELECT * FROM passages WHERE book_id=? AND spine_index=? ORDER BY seq",
            (book_id, spine_index),
        ).fetchall()
        return [self._passage_row(r, book_id) for r in rows]

    def get_summary(self, cache_key: str) -> dict | None:
        row = self.conn.execute("SELECT * FROM summaries WHERE cache_key=?", (cache_key,)).fetchone()
        return dict(row) if row else None

    def save_summary(
        self,
        *,
        cache_key: str,
        book_id: str,
        scope: str,
        spine_index: int | None,
        content_revision: str,
        prompt_version: str,
        model: str,
        title: str,
        text: str,
    ) -> None:
        self.conn.execute(
            "INSERT INTO summaries(cache_key, book_id, scope, spine_index, content_revision,"
            " prompt_version, model, title, text, generated_at) VALUES(?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(cache_key) DO UPDATE SET title=excluded.title, text=excluded.text,"
            " generated_at=excluded.generated_at",
            (
                cache_key,
                book_id,
                scope,
                spine_index,
                content_revision,
                prompt_version,
                model,
                title,
                text,
                _now(),
            ),
        )
        self.conn.commit()

    def passage_neighbors(self, book_id: str, passage_id: str, radius: int = 1) -> list[dict]:
        row = self.conn.execute(
            "SELECT spine_index, seq FROM passages WHERE book_id=? AND id=?", (book_id, passage_id)
        ).fetchone()
        if row is None:
            return []
        lo = max(0, int(row["seq"]) - max(0, radius))
        hi = int(row["seq"]) + max(0, radius)
        rows = self.conn.execute(
            "SELECT * FROM passages WHERE book_id=? AND spine_index=? AND seq BETWEEN ? AND ? ORDER BY seq",
            (book_id, row["spine_index"], lo, hi),
        ).fetchall()
        return [self._passage_row(r, book_id) for r in rows]

    def passage_exists(self, book_id: str, passage_id: str) -> bool:
        row = self.conn.execute("SELECT 1 FROM passages WHERE book_id=? AND id=?", (book_id, passage_id)).fetchone()
        return row is not None

    def search(self, book_id: str, query: str, limit: int = 6) -> list[dict]:
        query = (query or "").strip()
        if not query:
            return []
        terms = [t for t in re.split(r"\s+", query) if t]
        fts_terms = [t for t in terms if len(t) >= 3]
        rows: list[sqlite3.Row] = []
        if fts_terms:
            fts = " AND ".join('"' + t.replace('"', '""') + '"' for t in fts_terms)
            try:
                rows = self.conn.execute(
                    "SELECT p.*, bm25(passages_fts) AS rank FROM passages_fts"
                    " JOIN passages p ON p.rowid=passages_fts.rowid"
                    " WHERE passages_fts MATCH ? AND p.book_id=? ORDER BY rank LIMIT ?",
                    (fts, book_id, max(1, limit)),
                ).fetchall()
            except sqlite3.OperationalError:
                rows = []
        if not rows:
            # Trigram needs >= 3 characters; 1-2 char CJK queries fall back to
            # substring matching. This is also the safety net for FTS syntax
            # differences across SQLite builds.
            rows = self.conn.execute(
                "SELECT * FROM passages WHERE book_id=? AND text LIKE ? ORDER BY spine_index, seq LIMIT ?",
                (book_id, f"%{query}%", max(1, limit)),
            ).fetchall()
        return [self._passage_row(r, book_id) for r in rows]

    # ---------------------------------------------------------- annotations

    def list_annotations(self, device_id: str | None = None, limit: int = 100) -> list[dict]:
        sql = "SELECT * FROM annotations WHERE deleted=0"
        args: list[Any] = []
        if device_id:
            sql += " AND device_id=?"
            args.append(device_id)
        sql += " ORDER BY updated_at DESC LIMIT ?"
        args.append(max(1, min(500, limit)))
        rows = self.conn.execute(sql, args).fetchall()
        return [self._annotation_row(r) for r in rows]

    def get_annotation(self, annotation_id: str) -> dict | None:
        row = self.conn.execute("SELECT * FROM annotations WHERE id=?", (annotation_id,)).fetchone()
        return self._annotation_row(row) if row else None

    def get_ai_result(self, result_id: str) -> dict | None:
        row = self.conn.execute("SELECT * FROM ai_results WHERE id=?", (result_id,)).fetchone()
        return self._ai_result_row(row) if row else None

    def apply_sync_operation(self, device_id: str, op: dict) -> dict:
        """Apply one idempotent operation.

        Returns ``{op_id,status,server_id,error}`` where status is applied,
        duplicate, rejected or conflict. Rejected/conflict results are also
        stored, so retrying the same op_id returns the same decision; the
        client resolves a conflict with a new op_id.
        """
        op_id = str(op.get("op_id") or "").strip()
        op_type = str(op.get("type") or "").strip()
        if not op_id:
            return {"op_id": "", "status": "rejected", "server_id": "", "error": "missing op_id"}
        if op_type not in SYNC_TYPES:
            return self._record_op(device_id, op_id, op_type, "", "rejected", f"unknown type {op_type!r}")
        existing = self.conn.execute("SELECT * FROM sync_ops WHERE op_id=?", (op_id,)).fetchone()
        if existing is not None:
            # applied/duplicate are safe for the device to ack. rejected and
            # conflict are returned unchanged so the device does not drop a
            # record it still has to resolve.
            status = existing["result"]
            if status in ("applied", "duplicate"):
                status = "duplicate"
            return {
                "op_id": op_id,
                "status": status,
                "server_id": existing["entity_id"],
                "error": existing["error"] or None,
            }

        payload = op.get("payload")
        if not isinstance(payload, dict):
            return self._record_op(device_id, op_id, op_type, "", "rejected", "payload must be an object")
        try:
            if op_type in ("annotation.create", "note.create"):
                return self._apply_upsert(device_id, op_id, op_type, payload)
            if op_type in ("annotation.delete", "note.delete"):
                return self._apply_delete(device_id, op_id, op_type, payload)
            if op_type == "ai_result.create":
                return self._apply_ai_result(device_id, op_id, payload)
        except ValueError as exc:
            return self._record_op(device_id, op_id, op_type, "", "rejected", str(exc))
        return self._record_op(device_id, op_id, op_type, "", "rejected", "unhandled type")

    def pull_changes(self, device_id: str, cursor: int, limit: int) -> dict:
        rows = self.conn.execute(
            "SELECT * FROM sync_changes WHERE device_id=? AND seq>? ORDER BY seq LIMIT ?",
            (device_id, max(0, cursor), max(1, min(200, limit)) + 1),
        ).fetchall()
        has_more = len(rows) > max(1, min(200, limit))
        rows = rows[: max(1, min(200, limit))]
        changes = []
        for row in rows:
            entity = None
            if row["entity_type"] == "annotation":
                entity = self.get_annotation(row["entity_id"])
            elif row["entity_type"] == "ai_result":
                entity = self.get_ai_result(row["entity_id"])
            if entity is None:
                continue
            changes.append(
                {
                    "seq": row["seq"],
                    "entity_type": row["entity_type"],
                    "entity_id": row["entity_id"],
                    "op": row["op"],
                    "updated_at": row["updated_at"],
                    "entity": entity,
                }
            )
        next_cursor = str(rows[-1]["seq"]) if rows else str(max(0, cursor))
        return {"changes": changes, "next_cursor": next_cursor, "has_more": has_more}

    # ------------------------------------------------------------- internals

    def _apply_upsert(self, device_id: str, op_id: str, op_type: str, payload: dict) -> dict:
        entity_id = str(payload.get("id") or "").strip()
        if not entity_id:
            return self._record_op(device_id, op_id, op_type, "", "rejected", "missing payload.id")
        book_id = str(payload.get("book_id") or "").strip()
        if not book_id:
            return self._record_op(device_id, op_id, op_type, "", "rejected", "missing book_id")
        kind = str(payload.get("kind") or ("note" if op_type == "note.create" else "highlight")).strip()
        if kind not in _ANNOTATION_KINDS:
            kind = "note" if op_type == "note.create" else "highlight"
        quote = str(payload.get("quote") or "")
        note = str(payload.get("note") or payload.get("text") or "")
        passage_id = str(payload.get("passage_id") or "")
        anchor = payload.get("anchor") or {}
        if not isinstance(anchor, dict):
            anchor = {}
        now = _now()
        existing = self.conn.execute("SELECT * FROM annotations WHERE id=?", (entity_id,)).fetchone()
        if existing is not None:
            if existing["device_id"] != device_id:
                # V1 has per-device scopes; an id owned by another device is
                # not silently editable just because the id is known.
                return self._record_op(device_id, op_id, op_type, entity_id, "rejected", "entity belongs to another device")
            same = (
                existing["book_id"] == book_id
                and existing["passage_id"] == passage_id
                and existing["quote"] == quote
                and existing["note"] == note
                and existing["kind"] == kind
                and existing["deleted"] == 0
            )
            if same:
                return self._record_op(device_id, op_id, op_type, entity_id, "duplicate", "")
            # A different edit under the same id is a conflict; the client
            # must fetch/merge and send a new op_id.
            return self._record_op(device_id, op_id, op_type, entity_id, "conflict", "entity changed")
        with self.conn:
            self.conn.execute(
                "INSERT INTO annotations(id, device_id, book_id, passage_id, quote, note, kind,"
                " anchor_json, deleted, created_at, updated_at) VALUES(?,?,?,?,?,?,?,?,0,?,?)",
                (entity_id, device_id, book_id, passage_id, quote, note, kind, _json_dumps(anchor), now, now),
            )
            self.conn.execute(
                "INSERT INTO sync_changes(device_id, entity_type, entity_id, op, updated_at)"
                " VALUES(?,'annotation',?,'upsert',?)",
                (device_id, entity_id, now),
            )
        return self._record_op(device_id, op_id, op_type, entity_id, "applied", "")

    def _apply_delete(self, device_id: str, op_id: str, op_type: str, payload: dict) -> dict:
        entity_id = str(payload.get("id") or "").strip()
        if not entity_id:
            return self._record_op(device_id, op_id, op_type, "", "rejected", "missing payload.id")
        existing = self.conn.execute("SELECT * FROM annotations WHERE id=?", (entity_id,)).fetchone()
        if existing is None:
            return self._record_op(device_id, op_id, op_type, entity_id, "applied", "")
        if existing["device_id"] != device_id:
            return self._record_op(device_id, op_id, op_type, entity_id, "rejected", "entity belongs to another device")
        if existing["deleted"]:
            return self._record_op(device_id, op_id, op_type, entity_id, "duplicate", "")
        now = _now()
        with self.conn:
            self.conn.execute("UPDATE annotations SET deleted=1, updated_at=? WHERE id=?", (now, entity_id))
            self.conn.execute(
                "INSERT INTO sync_changes(device_id, entity_type, entity_id, op, updated_at)"
                " VALUES(?,'annotation',?,'delete',?)",
                (device_id, entity_id, now),
            )
        return self._record_op(device_id, op_id, op_type, entity_id, "applied", "")

    def _apply_ai_result(self, device_id: str, op_id: str, payload: dict) -> dict:
        entity_id = str(payload.get("id") or "").strip()
        if not entity_id:
            return self._record_op(device_id, op_id, "ai_result.create", "", "rejected", "missing payload.id")
        book_id = str(payload.get("book_id") or "").strip()
        if not book_id:
            return self._record_op(device_id, op_id, "ai_result.create", "", "rejected", "missing book_id")
        existing = self.conn.execute("SELECT * FROM ai_results WHERE id=?", (entity_id,)).fetchone()
        if existing is not None:
            if existing["device_id"] != device_id:
                return self._record_op(
                    device_id, op_id, "ai_result.create", entity_id, "rejected", "entity belongs to another device"
                )
            return self._record_op(device_id, op_id, "ai_result.create", entity_id, "duplicate", "")
        passage_ids = payload.get("passage_ids") or []
        segments = payload.get("segments") or []
        now = _now()
        with self.conn:
            self.conn.execute(
                "INSERT INTO ai_results(id, device_id, book_id, passage_ids, question, segments_json,"
                " deleted, created_at, updated_at) VALUES(?,?,?,?,?,?,0,?,?)",
                (
                    entity_id,
                    device_id,
                    book_id,
                    _json_dumps(passage_ids),
                    str(payload.get("question") or ""),
                    _json_dumps(segments),
                    now,
                    now,
                ),
            )
            self.conn.execute(
                "INSERT INTO sync_changes(device_id, entity_type, entity_id, op, updated_at)"
                " VALUES(?,'ai_result',?,'upsert',?)",
                (device_id, entity_id, now),
            )
        return self._record_op(device_id, op_id, "ai_result.create", entity_id, "applied", "")

    def _record_op(
        self, device_id: str, op_id: str, op_type: str, entity_id: str, status: str, error: str
    ) -> dict:
        now = _now()
        with self.conn:
            self.conn.execute(
                "INSERT OR IGNORE INTO sync_ops(op_id, device_id, type, entity_id, result, error, created_at)"
                " VALUES(?,?,?,?,?,?,?)",
                (op_id, device_id, op_type, entity_id, status, error, now),
            )
        return {"op_id": op_id, "status": status, "server_id": entity_id, "error": error or None}

    # ------------------------------------------------------------ row mappers

    @staticmethod
    def _book_row(row: sqlite3.Row) -> dict:
        error = None
        if row["error_code"] or row["error_message"]:
            error = {"code": row["error_code"], "message": row["error_message"]}
        return {
            "book_id": row["id"],
            "sha256": row["sha256"],
            "title": row["title"],
            "author": row["author"],
            "language": row["language"],
            "format": row["format"],
            "size": row["size"],
            "status": row["status"],
            "progress": row["progress"],
            "chapters": row["chapters"],
            "passages": row["passages"],
            "error": error,
            "created_at": row["created_at"],
            "updated_at": row["updated_at"],
        }

    def _passage_row(self, row: sqlite3.Row, book_id: str) -> dict:
        return {
            "passage_id": row["id"],
            "book_id": book_id,
            "spine_index": row["spine_index"],
            "chapter_title": self.chapter_title(book_id, row["spine_index"]),
            "seq": row["seq"],
            "kind": row["block_kind"],
            "block_index": row["block_index"],
            "text": row["text"],
            "char_start": row["char_start"],
            "char_end": row["char_end"],
        }

    @staticmethod
    def _annotation_row(row: sqlite3.Row) -> dict:
        return {
            "id": row["id"],
            "device_id": row["device_id"],
            "book_id": row["book_id"],
            "passage_id": row["passage_id"],
            "quote": row["quote"],
            "note": row["note"],
            "kind": row["kind"],
            "anchor": _json_loads(row["anchor_json"], {}),
            "deleted": bool(row["deleted"]),
            "created_at": row["created_at"],
            "updated_at": row["updated_at"],
        }

    @staticmethod
    def _ai_result_row(row: sqlite3.Row) -> dict:
        return {
            "id": row["id"],
            "device_id": row["device_id"],
            "book_id": row["book_id"],
            "passage_ids": _json_loads(row["passage_ids"], []),
            "question": row["question"],
            "segments": _json_loads(row["segments_json"], []),
            "deleted": bool(row["deleted"]),
            "created_at": row["created_at"],
            "updated_at": row["updated_at"],
        }
