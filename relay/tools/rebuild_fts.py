#!/usr/bin/env python3
"""Rebuild the passages FTS5 index from the authoritative passages table.

FTS5 external-content tables support the special 'rebuild' command, so this is
a single statement rather than a row-by-row reindex. Run it after a manual DB
repair, an SQLite upgrade, or if search results look stale.

Usage:
    python3 tools/rebuild_fts.py
    python3 tools/rebuild_fts.py --db /opt/reading-companion-relay/run/library/library.db
"""

from __future__ import annotations

import argparse
import os
import sqlite3
import sys
from pathlib import Path


def default_db() -> Path:
    base = os.environ.get("RELAY_LIBRARY_DIR") or "run/library"
    return Path(base) / "library.db"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default=str(default_db()), help="path to library.db")
    args = parser.parse_args()
    path = Path(args.db)
    if not path.is_file():
        print(f"database not found: {path}", file=sys.stderr)
        return 1
    conn = sqlite3.connect(path)
    try:
        conn.execute("INSERT INTO passages_fts(passages_fts) VALUES('rebuild')")
        conn.commit()
        count = conn.execute("SELECT count(*) FROM passages").fetchone()[0]
    except sqlite3.Error as exc:
        print(f"rebuild failed: {exc}", file=sys.stderr)
        return 1
    finally:
        conn.close()
    print(f"rebuilt passages_fts: {count} passages ({path})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
