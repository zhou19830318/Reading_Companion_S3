#!/usr/bin/env python3
"""Generate deterministic golden EPUBs for passage-ID and citation tests.

Run from anywhere:
    python3 relay/tests/fixtures/golden_books/generate.py

The generated files and golden_manifest.json are checked in as test data. The
manifest is produced from this script's own block model, not from the server
parser, so a parser regression cannot silently rewrite the expectation.
"""

from __future__ import annotations

import hashlib
import json
import struct
import unicodedata
import uuid
import zipfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ZIP_DATE = (1980, 1, 1, 0, 0, 0)

_CJK = "\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff"


def normalize(text: str) -> str:
    import re

    text = unicodedata.normalize("NFC", text)
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"\s+", " ", text)
    text = re.sub(rf"(?<=[{_CJK}])\s+(?=[{_CJK}])", "", text)
    text = re.sub(rf"(?<=[{_CJK}])\s+(?=[，。！？；：、）》」』】])", "", text)
    text = re.sub(rf"(?<=[（《「『【])\s+(?=[{_CJK}])", "", text)
    return text.strip()


def _png(width: int = 16, height: int = 16) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    rows = []
    for y in range(height):
        row = bytearray([0])  # filter type 0
        for x in range(width):
            row.append(0 if (x + y) % 2 == 0 else 255)
        rows.append(bytes(row))
    raw = b"".join(rows)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def _chapter_xhtml(title: str, blocks: list[tuple], lang: str) -> str:
    body = [f"<h1>{title}</h1>"]
    for block in blocks:
        kind = block[0]
        if kind == "p":
            body.append(f"<p>{block[1]}</p>")
        elif kind == "p_em":
            body.append(f"<p>{block[1]}<em>{block[2]}</em>{block[3]}</p>")
        elif kind == "li":
            body.append(f"<li>{block[1]}</li>")
        elif kind == "li_p":
            body.append(f"<li>{block[1]}<p>{block[2]}</p>{block[3]}</li>")
        elif kind == "div":
            body.append(f"<div>{block[1]}</div>")
        elif kind == "blockquote":
            body.append(f"<blockquote>{block[1]}</blockquote>")
        elif kind == "img":
            body.append(f'<p><img src="{block[1]}" alt="illustration"/></p>')
        elif kind == "raw":
            body.append(block[1])
        else:
            raise ValueError(f"unknown block {kind!r}")
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<!DOCTYPE html>\n'
        f'<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}" lang="{lang}">\n'
        f"<head><title>{title}</title><link rel=\"stylesheet\" href=\"style.css\"/></head>\n"
        "<body>\n" + "\n".join(body) + "\n</body>\n</html>\n"
    )


def _opf(
    name: str,
    title: str,
    author: str,
    language: str,
    chapter_files: list[str],
    *,
    epub3: bool,
    has_image: bool,
) -> str:
    uid = uuid.uuid5(uuid.NAMESPACE_URL, f"reading-companion-golden/{name}")
    items = [
        '<item id="style" href="style.css" media-type="text/css"/>',
    ]
    if has_image:
        items.append('<item id="pixel" href="images/pixel.png" media-type="image/png"/>')
    if epub3:
        items.append('<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>')
    else:
        items.append('<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>')
    for i, _ in enumerate(chapter_files):
        items.append(f'<item id="ch{i + 1}" href="{chapter_files[i]}" media-type="application/xhtml+xml"/>')
    spine_items = "\n".join(f'<itemref idref="ch{i + 1}"/>' for i in range(len(chapter_files)))
    toc_attr = "" if epub3 else ' toc="ncx"'
    modified = '<meta property="dcterms:modified">2026-11-19T00:00:00Z</meta>' if epub3 else ""
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        f'<package xmlns="http://www.idpf.org/2007/opf" version="{"3.0" if epub3 else "2.0"}" unique-identifier="bookid">\n'
        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        f'<dc:identifier id="bookid">urn:uuid:{uid}</dc:identifier>\n'
        f"<dc:title>{title}</dc:title>\n"
        f"<dc:creator>{author}</dc:creator>\n"
        f"<dc:language>{language}</dc:language>\n"
        f"{modified}\n"
        "</metadata>\n"
        "<manifest>\n" + "\n".join(items) + "\n</manifest>\n"
        f"<spine{toc_attr}>\n{spine_items}\n</spine>\n"
        "</package>\n"
    )


def _ncx(title: str, chapter_files: list[str], titles: list[str]) -> str:
    points = []
    for i, (href, chapter_title) in enumerate(zip(chapter_files, titles), start=1):
        points.append(
            f'<navPoint id="nav{i}" playOrder="{i}"><navLabel><text>{chapter_title}</text></navLabel>'
            f'<content src="{href}"/></navPoint>'
        )
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">\n'
        f"<head><meta name=\"dtb:uid\" content=\"golden\"/></head>\n"
        f"<docTitle><text>{title}</text></docTitle>\n"
        f"<navMap>{''.join(points)}</navMap>\n"
        "</ncx>\n"
    )


def _nav(chapter_files: list[str], titles: list[str]) -> str:
    links = "".join(f'<li><a href="{href}">{title}</a></li>' for href, title in zip(chapter_files, titles))
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<!DOCTYPE html>\n'
        '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">\n'
        "<head><title>目录</title></head><body>\n"
        f'<nav epub:type="toc" id="toc"><h1>目录</h1><ol>{links}</ol></nav>\n'
        "</body></html>\n"
    )


def _container() -> str:
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
        "</container>\n"
    )


def _write_zip(path: Path, entries: list[tuple[str, bytes]]) -> None:
    with zipfile.ZipFile(path, "w") as zf:
        info = zipfile.ZipInfo("mimetype", date_time=ZIP_DATE)
        info.compress_type = zipfile.ZIP_STORED
        zf.writestr(info, b"application/epub+zip")
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=ZIP_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zf.writestr(info, data)


BOOKS = [
    {
        "name": "zh_prose",
        "title": "山间书店",
        "author": "示例作者",
        "language": "zh",
        "epub3": False,
        "chapters": [
            (
                "第一章 山间",
                [
                    ("p", "山间的书店有一盏灯。"),
                    ("p", "灯下是一张旧木桌，桌面被许多人的手磨得发亮。"),
                    ("p", "店主把新到的书一本本拆开，纸页的味道慢慢铺满屋子。"),
                    ("p", "窗外的雨落在石阶上，声音很轻。"),
                    ("p", "他说，读书不是为了赶路，是为了在某个句子旁边停下来。"),
                    ("p", "我坐在靠窗的位置，翻开一本没有读完的旧书。"),
                    ("p_em", "书里夹着", "一片干枯的叶子", "，叶脉还很清楚。"),
                    ("p", "那一刻，时间像被折进了纸页。"),
                    ("p", "店主给我倒了一杯热茶，没有说话。"),
                    ("p", "我们各自读着，屋子里只剩下翻页的声音。"),
                ],
            ),
            (
                "第二章 灯下",
                [
                    ("p", "傍晚的时候，灯变得更亮了。"),
                    ("p", "一个孩子推门进来，问有没有讲星星的书。"),
                    ("p", "店主从最高的书架上取下一本，封面已经褪色。"),
                    ("p", "孩子坐在门槛上，一页一页地看。"),
                    ("p", "他读到一半，抬头问：星星为什么会发光？"),
                    ("p", "店主说，也许是因为它们也在读彼此。"),
                    ("p", "孩子笑了，把这个答案认真地记在本子上。"),
                    ("p", "雨停了，街道上有了脚步声。"),
                    ("p", "我把旧书合上，叶子仍夹在原处。"),
                    ("p", "离开时，那盏灯还亮着。"),
                ],
            ),
        ],
    },
    {
        "name": "zh_mixed",
        "title": "河边的信",
        "author": "示例作者",
        "language": "zh",
        "epub3": True,
        "chapters": [
            (
                "第一章 河边",
                [
                    ("p", "河边的风把信纸吹得发响。"),
                    ("p", "她把信折成一只小船，放进水里。"),
                    ("li", "第一件事：不要回头。"),
                    ("li", "第二件事：记住河的方向。"),
                    ("p", "小船顺流而下，穿过桥洞。"),
                    ("p", "桥上的人没有看见它。"),
                    ("p", "她想起小时候，祖母也这样放过一只船。"),
                    ("p_em", "祖母说，", "写下来的话会找到路", "。"),
                    ("p", "于是她把想说的话都写了下来。"),
                    ("p", "包括那些没有寄出的部分。"),
                ],
            ),
            (
                "第二章 回信",
                [
                    ("p", "三天后，她在门口捡到一只湿透的纸船。"),
                    ("p", "船上多了一行字：我收到了。"),
                    ("li", "字迹很轻，像是怕惊动什么。"),
                    ("li", "她把纸船放在窗台上晾干。"),
                    ("li_p", "她想，", "有些回信不需要地址", "也会抵达。"),
                    ("p", "窗外的河仍然向东流。"),
                    ("p", "她坐下来，开始写第二封信。"),
                    ("p", "这一次，她没有折成船。"),
                    ("p", "她把信夹进了一本书里。"),
                    ("p", "等下一个读到的人发现。"),
                ],
            ),
        ],
    },
    {
        "name": "en_prose",
        "title": "The Quiet Library",
        "author": "Example Author",
        "language": "en",
        "epub3": False,
        "chapters": [
            (
                "Chapter One: The Door",
                [
                    ("p", "The library opened at nine, but the door was already warm."),
                    ("p", "Mara had walked past it for years without looking in."),
                    ("p", "Today she stopped, because the rain had started."),
                    ("p", "Inside, a clock ticked more slowly than the one at home."),
                    ("p", "A librarian looked up and nodded, as if expecting her."),
                    ("p", "The shelves were tall enough to make the room feel like a forest."),
                    ("p_em", "She chose a book with a", "cracked blue spine", " and sat down."),
                    ("p", "The first page smelled of dust and oranges."),
                    ("p", "She read until the rain became a background sound."),
                    ("p", "When she looked up, the clock had moved only a little."),
                ],
            ),
            (
                "Chapter Two: The Margin",
                [
                    ("p", "In the margin of page twelve, someone had written a note."),
                    ("p", "It said: this is where the story begins for me."),
                    ("p", "Mara turned the page and found another note."),
                    ("p", "Each one was short, like a hand reaching across time."),
                    ("p", "She borrowed a pencil and added her own sentence."),
                    ("p", "The librarian pretended not to notice."),
                    ("p", "At closing time, Mara returned the book to the shelf."),
                    ("p", "She left her note inside, for the next reader."),
                    ("p", "The rain had stopped, but the street shone."),
                    ("p", "She walked home slowly, carrying the quiet with her."),
                ],
            ),
        ],
    },
    {
        "name": "en_dialogue",
        "title": "A Conversation About Maps",
        "author": "Example Author",
        "language": "en",
        "epub3": True,
        "chapters": [
            (
                "Chapter One: The Table",
                [
                    ("p", "\"You are late,\" said Ivo, without looking up."),
                    ("p", "\"The tram stopped,\" said Lena. \"Twice.\""),
                    ("p", "He moved a cup so she could put down her map."),
                    ("li", "The map was older than the city."),
                    ("li", "It showed a river where the road now ran."),
                    ("p", "\"Why do you keep it?\" she asked."),
                    ("p", "\"Because it remembers something we forgot.\""),
                    ("p", "Lena traced the river with her finger."),
                    ("p", "The paper was soft at the folds."),
                    ("p", "\"Maybe we can walk it anyway,\" she said."),
                ],
            ),
            (
                "Chapter Two: The Walk",
                [
                    ("p", "They followed the old line through the new streets."),
                    ("p", "At the first corner, a bakery stood where a bridge had been."),
                    ("li", "Ivo bought two rolls and gave her one."),
                    ("li", "They ate while the traffic passed."),
                    ("p", "\"Do you think the river misses itself?\" asked Lena."),
                    ("p", "Ivo laughed, then considered it seriously."),
                    ("p_em", "\"I think", "places remember", " the people who loved them.\""),
                    ("p", "They walked until the map ended at a blank edge."),
                    ("p", "Beyond it, the city continued without instruction."),
                    ("p", "They stood there for a while, then turned back."),
                ],
            ),
        ],
    },
    {
        "name": "illustrated",
        "title": "The Lantern and the Map",
        "author": "Example Author",
        "language": "en",
        "epub3": True,
        "cover": True,
        "chapters": [
            (
                "Chapter One: The Lantern",
                [
                    ("p", "The lantern hung above the door all winter."),
                    ("img", "images/pixel.png"),
                    ("p", "It was not bright, but it was steady."),
                    ("p", "Travelers used it to find the inn."),
                    ("li", "One traveler arrived with no luggage."),
                    ("li", "Another arrived with a map and no destination."),
                    ("p", "The innkeeper gave them both soup."),
                    ("p", "At midnight, the lantern flickered once."),
                ],
            ),
            (
                "Chapter Two: The Map",
                [
                    ("p", "The map showed a road that ended at the sea."),
                    ("p", "Someone had drawn a small boat at the edge."),
                    ("p", "The traveler with no luggage laughed."),
                    ("li", "The innkeeper asked what was funny."),
                    ("li", "The traveler said the boat was theirs."),
                    ("p", "By morning, the map was gone."),
                    ("p", "So was the traveler."),
                    ("p", "Only the lantern remained, still burning."),
                ],
            ),
        ],
    },
]


def build_book(spec: dict) -> tuple[list[tuple[str, bytes]], list[dict]]:
    name = spec["name"]
    chapter_files = [f"ch{i + 1}.xhtml" for i in range(len(spec["chapters"]))]
    chapter_titles = [c[0] for c in spec["chapters"]]
    entries: list[tuple[str, bytes]] = [
        ("META-INF/container.xml", _container().encode()),
        (
            "OEBPS/content.opf",
            _opf(
                name,
                spec["title"],
                spec["author"],
                spec["language"],
                chapter_files,
                epub3=spec["epub3"],
                has_image=any(b[0] == "img" for _, blocks in spec["chapters"] for b in blocks),
            ).encode(),
        ),
        ("OEBPS/style.css", b"body { margin: 1em; } h1 { font-size: 1.4em; }\n"),
    ]
    if spec["epub3"]:
        entries.append(("OEBPS/nav.xhtml", _nav(chapter_files, chapter_titles).encode()))
    else:
        entries.append(("OEBPS/toc.ncx", _ncx(spec["title"], chapter_files, chapter_titles).encode()))
    if any(b[0] == "img" for _, blocks in spec["chapters"] for b in blocks):
        entries.append(("OEBPS/images/pixel.png", _png()))

    for i, (title, blocks) in enumerate(spec["chapters"]):
        entries.append((f"OEBPS/{chapter_files[i]}", _chapter_xhtml(title, blocks, spec["language"]).encode()))

    if spec.get("cover"):
        entries.append(
            ("OEBPS/cover.xhtml", _chapter_xhtml("Cover", [("div", "The Lantern and the Map")], "en").encode())
        )
        # The cover is deliberately not in the spine: it tests non-spine files
        # and keeps spine indices stable for the expected manifest.

    # Expected passages are built from the input model, not the parser.
    expected: list[dict] = []
    for spine_index, (_, blocks) in enumerate(spec["chapters"]):
        p_count = 0
        l_count = 0
        seq = 0

        def add(kind: str, index: int, text: str) -> None:
            nonlocal seq
            text = normalize(text)
            if not text:
                return
            expected.append(
                {
                    "passage_id": f"{spine_index}:{kind}{index}",
                    "spine_index": spine_index,
                    "kind": kind,
                    "block_index": index,
                    "text": text,
                    "seq": seq,
                }
            )
            seq += 1

        for block in blocks:
            kind = block[0]
            if kind == "p":
                add("p", p_count, block[1])
                p_count += 1
            elif kind == "p_em":
                add("p", p_count, block[1] + block[2] + block[3])
                p_count += 1
            elif kind == "li":
                add("l", l_count, block[1])
                l_count += 1
            elif kind == "li_p":
                add("l", l_count, block[1] + block[3])
                l_count += 1
                add("p", p_count, block[2])
                p_count += 1
            elif kind == "img":
                # An image-only p is counted by the parser but has no text, so
                # it is not a passage. The p counter still advances.
                p_count += 1
    return entries, expected


def main() -> None:
    manifest = {"generated_by": "generate.py", "rule": "docs/ai-native/api-v1.md", "books": []}
    for spec in BOOKS:
        path = HERE / f"{spec['name']}.epub"
        entries, expected = build_book(spec)
        _write_zip(path, entries)
        data = path.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        book_id = sha[:16]
        for p in expected:
            p["passage_id"] = f"{book_id}:{p['passage_id']}"
        manifest["books"].append(
            {
                "file": path.name,
                "sha256": sha,
                "book_id": book_id,
                "title": spec["title"],
                "author": spec["author"],
                "language": spec["language"],
                "chapters": [
                    {"spine_index": i, "title": title}
                    for i, (title, _) in enumerate(spec["chapters"])
                ],
                "passage_count": len(expected),
                "passages": expected,
            }
        )
        print(f"{path.name}: {sha[:16]}… {len(expected)} passages")
    (HERE / "golden_manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
