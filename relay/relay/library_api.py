"""Book upload persistence shared by the device API and the admin workbench.

Both entry points must apply the same rules: extension allow-list, byte limit
while streaming, SHA-256 dedupe, and one temporary file per attempt. Errors are
raised as ``UploadError`` so each HTTP layer only maps status/code.
"""

from __future__ import annotations

import asyncio
import hashlib
import os
import uuid
from pathlib import Path

from starlette.datastructures import UploadFile

from . import book_ingest
from .library_store import LibraryStore

ALLOWED_SUFFIXES = (".epub", ".txt")


class UploadError(Exception):
    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message


async def store_upload(
    store: LibraryStore,
    upload: UploadFile,
    *,
    title: str,
    author: str,
    max_bytes: int,
) -> tuple[dict, bool, Path | None]:
    """Persist one upload and register it.

    Returns ``(book, duplicate, path)``. ``path`` is None for a duplicate, so
    the caller does not re-ingest. Raises UploadError for client errors.
    """
    filename = Path(upload.filename or "").name
    suffix = Path(filename).suffix.lower()
    if suffix not in ALLOWED_SUFFIXES:
        raise UploadError(415, "UNSUPPORTED_FORMAT", "only .epub and .txt are supported")
    if not max_bytes:
        raise UploadError(413, "BOOK_TOO_LARGE", "book uploads are disabled")

    tmp = store.books_dir / f".upload-{uuid.uuid4().hex}{suffix}"
    digest = hashlib.sha256()
    size = 0
    try:
        with tmp.open("wb") as out:
            while True:
                chunk = await upload.read(1024 * 1024)
                if not chunk:
                    break
                size += len(chunk)
                if size > max_bytes:
                    raise UploadError(413, "BOOK_TOO_LARGE", f"book exceeds {max_bytes} bytes")
                digest.update(chunk)
                out.write(chunk)
    except UploadError:
        tmp.unlink(missing_ok=True)
        raise
    except Exception as exc:  # disk full, client disconnect, ...
        tmp.unlink(missing_ok=True)
        raise UploadError(400, "BAD_REQUEST", "upload failed") from exc
    finally:
        await upload.close()

    if size == 0:
        tmp.unlink(missing_ok=True)
        raise UploadError(400, "BAD_REQUEST", "empty file")

    sha256 = digest.hexdigest()
    existing = store.get_book_by_hash(sha256, include_deleted=True)
    if existing is not None:
        # A soft-deleted row still owns the hash and book id. Re-uploading the
        # same bytes restores it instead of failing on the UNIQUE constraint.
        final = store.books_dir / f"{sha256}{suffix}"
        restored = store.restore_book(
            existing["book_id"],
            title=(title or Path(filename).stem)[:500],
            author=(author or "")[:500],
        )
        if restored is None:
            tmp.unlink(missing_ok=True)
            return existing, True, None
        os.replace(tmp, final)
        store.set_book_file(restored["book_id"], final)
        return restored, False, final

    try:
        book = store.create_book(
            sha256=sha256,
            title=(title or Path(filename).stem)[:500],
            author=(author or "")[:500],
            language="",
            book_format=suffix.lstrip("."),
            size=size,
            file_path="",
        )
    except ValueError as exc:
        tmp.unlink(missing_ok=True)
        raise UploadError(409, "BOOK_ID_COLLISION", str(exc)) from exc

    final = store.books_dir / f"{sha256}{suffix}"
    os.replace(tmp, final)
    store.set_book_file(book["book_id"], final)
    return store.get_book(book["book_id"]) or book, False, final


async def start_ingest(
    store: LibraryStore,
    book_id: str,
    path: Path,
    *,
    sync: bool,
    background_tasks=None,
) -> None:
    """Ingest inline (tests) or in FastAPI's background worker (production)."""
    if sync:
        await asyncio.to_thread(book_ingest.ingest_book, store, book_id, path)
    elif background_tasks is not None:
        background_tasks.add_task(book_ingest.ingest_book, store, book_id, path)
    else:
        await asyncio.to_thread(book_ingest.ingest_book, store, book_id, path)
