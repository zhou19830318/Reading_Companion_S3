#!/usr/bin/env python3
"""Web remote for the Reading Companion S3 e-ink reader.

Owns the reader's serial port and exposes it as a small web service: the page
shows the live e-ink frame (CMD:SCREENSHOT, FNV-1a CRC checked), injects
hardware keys (CMD:KEY / CMD:KEYHOLD), and refreshes the frame whenever the
firmware reports a completed screen update ([DISP]), whatever caused it.
All device traffic goes through the serial link; nothing else talks to the
reader while this server runs.

  python3 webremote/server.py [--port /dev/ttyACM0] [--http 8090] [--host auto]

By default the server listens on all interfaces and prints the machine's LAN
address (e.g. http://192.168.2.11:8090), so phones and tablets on the same
network can open the console without knowing or typing the IP.

Endpoints: /api/state /api/key /api/shot /api/info /api/reconnect
           /api/screen.png?seq=N&orient=land|port   /api/events (SSE)
"""

import argparse
import json
import os
import queue
import re
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

# Portable bundle: a vendored pyserial next to this file wins over any system
# install, so the unpacked folder runs on a PC with nothing preinstalled.
_VENDOR = Path(__file__).resolve().parent / "vendor"
if _VENDOR.is_dir():
    sys.path.insert(0, str(_VENDOR))

import serial
from serial.tools import list_ports

try:
    import zlib
except ImportError:  # bare embedded interpreters: stored-deflate fallback below
    zlib = None

KEYS = ("back", "ok", "left", "right", "up", "down", "power")
TOKEN_MASK = re.compile(r"(RELAY\s+\S+\|)\S+")
# Firmware echoes one of these before the press edge it produced, so the bridge
# can tell its own CMD:KEY injections apart from taps on the device buttons.
INJECTED_KEY_RE = re.compile(r"^KEY(?:HOLD)?\s+\S+(?:\s+\d+\s+ms)?\s+injected$")
PRESS_EDGE = "[IN] press edge @"
# Firmware logs one of these after each render job completes, whatever moved
# the page: device buttons, injected CMD:KEY, tilt, auto turn or a menu.
SCREEN_UPDATE = "[DISP] frame"
STATIC = Path(__file__).resolve().parent / "static"
LOG_RING = 4000


class Bridge:
    """Serial owner: one reader thread, serialized writes and captures."""

    def __init__(self, port_path: str):
        self.port_path = port_path
        self.ser = None
        self.connected = False
        self.stop = False
        self.write_lock = threading.Lock()
        self.cap_lock = threading.Lock()
        self.state_lock = threading.Lock()
        self.seq = 0
        self.crc_ok = False
        self.capture_ms = 0
        self.last_key = ""
        self.png = {"port": None, "land": None}
        self.lines = []
        self.subs = []
        self.shot_done = threading.Event()
        self.shot_err = ""
        self.info_line = None
        self.info_done = threading.Event()
        self.voice_line = None
        self.voice_done = threading.Event()
        self._expecting = 0
        self._shot = b""
        self._shot_ts = 0.0
        self._resync = False
        self._want_crc = None
        self._buf = b""
        self._gen = 0
        self._gen_lock = threading.Lock()
        self.frame_crc = None
        self._pub_crc = None
        # Physical-key -> web refresh. The firmware logs every press edge, but
        # injected CMD:KEY presses log too; remember when we last injected so
        # those edges are not mistaken for device buttons.
        self._inject_lock = threading.Lock()
        self._inject_log_ts = 0.0
        self._phys_lock = threading.Lock()
        self._phys_gen = 0
        # Screen-change -> web refresh. [DISP] covers every input source, so a
        # burst of repaints is coalesced into one capture of the settled frame.
        self._screen_lock = threading.Lock()
        self._screen_gen = 0
        self.thread = threading.Thread(target=self._run, daemon=True, name="serial-reader")
        self.thread.start()

    # -- pub/sub ----------------------------------------------------------
    def publish(self, event: str, data):
        for q in list(self.subs):
            try:
                q.put_nowait((event, data))
            except queue.Full:
                pass  # slow client: backlog on resubscribe covers the gap

    def subscribe(self):
        q = queue.Queue(2000)
        self.subs.append(q)
        return q

    def unsubscribe(self, q):
        if q in self.subs:
            self.subs.remove(q)

    def _line(self, line: str):
        line = TOKEN_MASK.sub(r"\1***", line)
        with self.state_lock:
            self.lines.append(line)
            if len(self.lines) > LOG_RING:
                del self.lines[: len(self.lines) - LOG_RING]
        if line.startswith("CLOUD "):
            self.info_line = line
            self.info_done.set()
        if line.startswith(("VOICE_TX ", "VOICE_RX")):
            self.voice_line = line
            self.voice_done.set()
        if INJECTED_KEY_RE.match(line):
            # The injected press edge follows this line in the same loop pass.
            with self._inject_lock:
                self._inject_log_ts = time.time()
        elif PRESS_EDGE in line:
            with self._inject_lock:
                injected = time.time() - self._inject_log_ts < 0.15
            if not injected:
                self.last_key = "physical"
                self._schedule_physical_refresh()
        if SCREEN_UPDATE in line:
            # Any settled repaint — physical key, CMD:KEY, tilt page turn,
            # auto turn, menu transition — becomes a fresh web frame.
            self._schedule_screen_refresh()
        self.publish("log", line)

    def _schedule_screen_refresh(self):
        # A capture already in flight sends CMD:SCREENSHOT; the firmware waits
        # for the render task to go idle before dumping, so that frame already
        # includes this repaint. Queueing another dump behind it would only
        # double the serial work.
        if self.cap_lock.locked():
            return
        with self._screen_lock:
            self._screen_gen += 1
            gen = self._screen_gen
        threading.Thread(target=self._screen_refresh, args=(gen,), daemon=True,
                         name="screen-refresh").start()

    def _screen_refresh(self, gen: int):
        # Let a page-turn animation or a short menu repaint sequence settle;
        # every new [DISP] bumps the generation and supersedes this timer.
        time.sleep(0.18)
        with self._screen_lock:
            if gen != self._screen_gen:
                return
        if not self.connected:
            return
        if self.cap_lock.locked():
            # An explicit /api/key or /api/shot capture is already in flight and
            # sends CMD:SCREENSHOT, which waits for the render task to settle.
            return
        try:
            self.capture(timeout=8.0)
        except Exception:
            # Next [DISP] retries; a failed idle capture must not kill the
            # serial reader or leave the screen hook wedged.
            pass

    def _schedule_physical_refresh(self):
        # Physical keys bypass the HTTP API entirely. Publish the key edge for
        # the status bar; the [DISP] marker that follows the repaint drives the
        # actual screenshot. Coalesce a burst of taps.
        with self._phys_lock:
            self._phys_gen += 1
            gen = self._phys_gen
        threading.Thread(target=self._physical_refresh, args=(gen,), daemon=True,
                         name="physical-refresh").start()

    def _physical_refresh(self, gen: int):
        time.sleep(0.12)
        with self._phys_lock:
            if gen != self._phys_gen or not self.connected:
                return
        self.publish("key", {"key": "", "source": "device"})

    # -- reader thread ----------------------------------------------------
    def _run(self):
        while not self.stop:
            try:
                self.ser = serial.Serial(self.port_path, 115200, timeout=0.1, write_timeout=15)
            except Exception as exc:
                self._set_connected(False, str(exc))
                time.sleep(2.0)
                continue
            self._buf = b""
            self._expecting = 0
            self._resync = False
            self._shot_ts = 0.0
            self._set_connected(True, "")
            try:
                self._pump()
            except Exception as exc:
                self._set_connected(False, str(exc))
            finally:
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None
            if not self.stop:
                time.sleep(1.0)

    def _set_connected(self, ok: bool, err: str):
        with self.state_lock:
            changed = self.connected != ok
            self.connected = ok
        if changed or err:
            self.publish("status", {"connected": ok, "port": self.port_path, "error": err})
        if ok and changed:
            threading.Thread(target=self._initial_capture, daemon=True).start()

    def _initial_capture(self):
        # opening the port resets the ESP32; early SCREENSHOT commands are lost
        time.sleep(3.0 if self.seq == 0 else 2.0)
        for _ in range(4):
            try:
                self.capture(timeout=6.0)
                # Push the boot frame: clients otherwise keep showing the frame
                # from before the reset until the next manual key/shot.
                with self.state_lock:
                    seq, crc, ms, ok = self.seq, self._pub_crc, self.capture_ms, self.crc_ok
                if ok:
                    self.publish("screen", {"seq": seq, "crcOk": True, "ms": ms, "crc": f"{crc:08X}"})
                return
            except Exception:
                time.sleep(1.0)

    def _pump(self):
        while not self.stop:
            data = self.ser.read(4096)
            if not data:
                if self._expecting and time.time() - self._shot_ts > 3.0:
                    self._abandon_shot()
                continue
            self._buf += data
            if self._expecting:
                self._shot_ts = time.time()
            while True:
                if self._expecting:
                    need = self._expecting - len(self._shot)
                    take = min(need, len(self._buf))
                    if take:
                        self._shot += self._buf[:take]
                        self._buf = self._buf[take:]
                    if len(self._shot) < self._expecting:
                        break
                    self._expecting = 0
                    self._finish_shot()
                    continue
                idx = self._buf.find(b"\n")
                if idx < 0:
                    break
                raw, self._buf = self._buf[:idx], self._buf[idx + 1:]
                line = raw.decode("utf-8", "replace").rstrip("\r")
                if self._resync:
                    # Tail of an abandoned frame arrives as garbage "lines";
                    # drop them until a clean one shows up, otherwise they
                    # swallow CMD replies the tour assertions wait for.
                    if "\ufffd" in line:
                        continue
                    self._resync = False
                if line.startswith("SCREENSHOT_START:"):
                    self._expecting = int(line.split(":")[1])
                    self._shot = b""
                    self._shot_ts = time.time()
                    continue
                if line.startswith("FBPTR:") and "CRC:" in line:
                    self._want_crc = int(line.split("CRC:")[1].strip(), 16)
                    continue
                if line == "SCREENSHOT_END" or line.startswith("SCREENSHOT_SENT:"):
                    continue
                self._line(line)

    def _abandon_shot(self):
        self._expecting = 0
        self._shot = b""
        self._resync = True
        self.crc_ok = False
        self.shot_err = "screenshot frame stalled"
        self.shot_done.set()
        self._line("[server] screenshot frame abandoned (RX stall)")

    def _finish_shot(self):
        crc = 2166136261
        for b in self._shot:
            crc = ((crc ^ b) * 16777619) & 0xFFFFFFFF
        ok = self._want_crc is None or crc == self._want_crc
        self.crc_ok = ok
        if ok:
            self.frame_crc = crc
            # firmware dumps the panel in scan order (800x480); the readable
            # portrait view is that buffer rotated -90, as in device_test.py
            native = self._shot
            port = _rotate_port(native)
            with self.state_lock:
                self.seq += 1
                seq = self.seq
                self.png["port"] = _png_1bit(480, 800, port, PORT_STRIDE)
                self.png["land"] = _png_1bit(800, 480, native, NATIVE_STRIDE)
            self.shot_err = ""
            if crc != self._pub_crc:
                self._pub_crc = crc
                self.publish("screen", {"seq": seq, "crcOk": True, "ms": self.capture_ms, "crc": f"{crc:08X}"})
        else:
            self.shot_err = f"CRC mismatch: got {crc:08X} want {self._want_crc:08X}"
            self.publish("screen", {"seq": self.seq, "crcOk": False, "ms": self.capture_ms, "error": self.shot_err})
        self.shot_done.set()

    # -- commands ---------------------------------------------------------
    def send(self, cmd: str):
        if not self.connected or self.ser is None:
            raise RuntimeError("serial port not connected")
        with self.write_lock:
            self.ser.write(f"CMD:{cmd}\n".encode())
            self.ser.flush()

    def capture(self, timeout=32.0):
        with self.cap_lock:
            # First attempt outlives a blocking relay HTTP call on the device's
            # loop task (up to 30 s); later attempts only cover e-ink tearing.
            for attempt, wait_s in enumerate((timeout, 10.0, 10.0)):
                self.shot_done.clear()
                self.crc_ok = False
                t0 = time.time()
                self.send("SCREENSHOT")
                if not self.shot_done.wait(wait_s):
                    raise RuntimeError("screenshot timed out")
                self.capture_ms = int((time.time() - t0) * 1000)
                if self.crc_ok:
                    return self.seq
                # e-ink refresh tears the frame; land the retry outside it
                time.sleep(0.9)
            raise RuntimeError(self.shot_err or "screenshot corrupt")

    def press(self, key: str, hold_ms: int, settle_ms: int, shot: bool, follow_ms: int = 0):
        # follow_ms is an optional polling fallback; the default is 0 because
        # screen changes are delivered by the firmware's [DISP] marker.
        if key not in KEYS:
            raise ValueError(f"unknown key {key!r}")
        with self._inject_lock:
            # Covers the gap before the firmware's "KEY ... injected" echo; the
            # echo refreshes it again, so a slow command queue cannot make the
            # resulting press edge look physical.
            self._inject_log_ts = time.time()
        if hold_ms > 0:
            self.send(f"KEYHOLD {key} {hold_ms}")
        else:
            self.send(f"KEY {key}")
        self.last_key = f"{key}{' hold ' + str(hold_ms) + 'ms' if hold_ms else ''}"
        self.publish("key", {"key": key, "holdMs": hold_ms})
        # let the render task publish the new frame before grabbing it
        time.sleep(max(settle_ms, 0) / 1000.0 + (hold_ms / 1000.0 if hold_ms else 0))
        seq = self.capture() if shot else self.seq
        if shot:
            # Off by default: [DISP] already refreshes every settled screen.
            # A caller can still opt into polling for old firmware or a repaint
            # source that does not emit the marker.
            self._follow_start(follow_ms)
        return {"ok": True, "seq": seq, "crcOk": self.crc_ok, "crc": f"{self.frame_crc:08X}" if self.frame_crc is not None else None,
                "captureMs": self.capture_ms, "lastKey": self.last_key}

    def _follow_start(self, follow_ms: int):
        if follow_ms <= 0:
            return
        with self._gen_lock:
            self._gen += 1
            gen = self._gen
        threading.Thread(
            target=self._follow_frames, args=(gen, follow_ms / 1000.0),
            daemon=True, name="frame-follow",
        ).start()

    def _follow_frames(self, gen: int, budget: float, gap: float = 0.9):
        # menus often repaint seconds after the key (wifi join, book parse):
        # keep capturing for the whole window; unchanged frames publish nothing
        end = time.time() + budget
        while time.time() < end and gen == self._gen:
            try:
                self.capture(timeout=8.0)
            except Exception:
                return
            time.sleep(gap)

    def info(self):
        # the device main loop can stall seconds behind wifi management
        for _ in range(2):
            self.info_done.clear()
            self.info_line = None
            self.send("CLOUD")
            if self.info_done.wait(6.0):
                break
        else:
            raise RuntimeError("no CLOUD reply")
        out = {}
        for part in self.info_line.split()[1:]:
            if "=" in part:
                k, v = part.split("=", 1)
                out[k] = v
        return out

    # -- voice input (text / external PCM) ---------------------------------
    def _voice_wait(self, timeout=8.0):
        if not self.voice_done.wait(timeout):
            raise RuntimeError("no voice command reply")
        return self.voice_line

    def text(self, text: str, follow_ms: int = 20000):
        # serial is line-based: collapse newlines/whitespace into single spaces
        clean = " ".join(text.split())
        if not clean:
            raise ValueError("empty text")
        if len(clean.encode("utf-8")) > 400:
            raise ValueError("text too long (400 bytes max)")
        for _ in range(2):
            self.voice_done.clear()
            self.voice_line = None
            self.send(f"VOICE_TX {clean}")
            try:
                line = self._voice_wait()
                break
            except RuntimeError:
                continue
        else:
            raise RuntimeError("no VOICE_TX reply")
        accepted = "accepted" in line
        if accepted:
            self._follow_start(follow_ms)
        return {"ok": True, "accepted": accepted, "reply": line}

    def audio(self, pcm: bytes, follow_ms: int = 20000):
        if not pcm or len(pcm) % 2 or len(pcm) > 16000 * 20 * 2:
            raise ValueError("pcm must be 1..640000 even bytes (16 kHz mono int16, 20 s cap)")
        samples = len(pcm) // 2
        line = ""
        for _attempt in range(4):
            self.voice_done.clear()
            self.voice_line = None
            self.send(f"VOICE_RX {samples}")
            line = self._voice_wait()
            if "ready" in line or "not idle voice screen" not in line:
                break
            # WiFi idle-shutdown may have parked the radio; re-entering the
            # voice screen runs the saved-network auto-connect detour.
            self.send("GO_VOICE")
            time.sleep(6)
        if "ready" not in line:
            return {"ok": True, "accepted": False, "samples": samples, "reply": line}
        with self.write_lock:
            # Paced ~20 KB/s: the USB-Serial-JTAG 64 B hardware FIFO drops
            # bytes on ISR latency spikes at full bulk rate (observed: ~100 KB
            # of a 181 KB burst lost, upload never completes).
            for off in range(0, len(pcm), 512):
                self.ser.write(pcm[off:off + 512])
                time.sleep(0.025)
            self.ser.flush()
        self.voice_done.clear()
        self.voice_line = None
        self.send("VOICE_RX_END")
        line = self._voice_wait()
        accepted = "accepted" in line
        if accepted:
            self._follow_start(follow_ms)
            return {"ok": True, "accepted": True, "samples": samples, "reply": line}
        # END rejected usually means the relay session died mid-upload (e.g.
        # ASR_QUOTA_EXHAUSTED) and the recorder left the capturing state.
        # Surface the device-side reason so the page can explain the refusal.
        reason = ""
        with self.state_lock:
            for l in reversed(self.lines[-60:]):
                if "relay error" in l or "[VOICE] failed:" in l:
                    reason = l.split("] ", 2)[-1]
                    break
        return {"ok": True, "accepted": False, "samples": samples, "reply": line, "reason": reason}

    def reconnect(self):
        with self.write_lock:
            if self.ser is not None:
                try:
                    self.ser.close()
                except Exception:
                    pass
        return {"ok": True}

    # -- port selection (cross-platform bundle) ------------------------------
    def ports(self):
        out = []
        for p in list_ports.comports():
            out.append({"device": p.device, "desc": p.description or "", "vid": p.vid, "pid": p.pid})
        # USB adapters first: onboard ttyS* clutter would bury the e-book port.
        out.sort(key=lambda p: (p["vid"] is None, p["device"]))
        return {"ok": True, "ports": out, "current": self.port_path}

    def connect_port(self, path: str):
        path = (path or "").strip()
        if not path:
            raise ValueError("empty port")
        with self.write_lock:
            self.port_path = path
            if self.ser is not None:
                try:
                    self.ser.close()
                except Exception:
                    pass
        return {"ok": True, "port": path}

    def debug(self, cmd: str):
        # allowlisted navigation hooks used by the function tour
        if cmd not in ("GO_HOME", "GO_VOICE", "GO_CALENDAR", "GO_SETTINGS", "PRE_OFF", "PRE_ON", "PRE_DUMP",
                       "SYNC_STATUS", "SYNC_PUSH"):
            raise ValueError("command not allowed")
        self.send(cmd)
        return {"ok": True}

    def state(self):
        with self.state_lock:
            return {
                "connected": self.connected,
                "port": self.port_path,
                "seq": self.seq,
                "hasScreen": self.png["port"] is not None,
                "crcOk": self.crc_ok,
                "crc": f"{self.frame_crc:08X}" if self.frame_crc is not None else None,
                "captureMs": self.capture_ms,
                "lastKey": self.last_key,
                "keys": list(KEYS),
            }


# -- 1-bit PNG codec (stdlib only, so the bundle runs on any bare Python) ----
NATIVE_STRIDE = 100  # 800 px packed MSB-first
PORT_STRIDE = 60     # 480 px
# portrait(px, py) = native(x=py, y=479-px): precomputed row offsets per byte
_ROT_PAIRS = [
    tuple(((479 - (ob * 8 + k)) * NATIVE_STRIDE, 0x80 >> k) for k in range(8))
    for ob in range(PORT_STRIDE)
]


def _rotate_port(native: bytes) -> bytes:
    out = bytearray(PORT_STRIDE * 800)
    for py in range(800):
        bi = py >> 3
        mask = 1 << (7 - (py & 7))
        base = py * PORT_STRIDE
        for ob in range(PORT_STRIDE):
            acc = 0
            for off, bit in _ROT_PAIRS[ob]:
                if native[off + bi] & mask:
                    acc |= bit
            out[base + ob] = acc
    return bytes(out)


_CRC_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (0xEDB88320 if _c & 1 else 0)
    _CRC_TABLE.append(_c)


def _crc32(data: bytes) -> int:
    if zlib is not None:
        return zlib.crc32(data) & 0xFFFFFFFF
    crc = 0xFFFFFFFF
    for b in data:
        crc = _CRC_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def _adler32(data: bytes) -> int:
    a, b = 1, 0
    for x in data:
        a = (a + x) % 65521
        b = (b + a) % 65521
    return (b << 16) | a


def _stored_deflate(raw: bytes) -> bytes:
    out = bytearray(b"\x78\x01")
    for off in range(0, len(raw), 65535):
        block = raw[off:off + 65535]
        out.append(1 if off + 65535 >= len(raw) else 0)
        out += struct.pack("<HH", len(block), len(block) ^ 0xFFFF)
        out += block
    out += struct.pack(">I", _adler32(raw))
    return bytes(out)


def _png_1bit(width: int, height: int, rows: bytes, stride: int) -> bytes:
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter: none
        raw += rows[y * stride:(y + 1) * stride]
    raw = bytes(raw)
    body = zlib.compress(raw, 6) if zlib is not None else _stored_deflate(raw)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", _crc32(tag + data))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0))
            + chunk(b"IDAT", body)
            + chunk(b"IEND", b""))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    bridge: Bridge = None  # set by main()

    def log_message(self, *args):
        pass

    # -- helpers ----------------------------------------------------------
    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def _fail(self, exc, code=500):
        self._json({"ok": False, "error": str(exc)}, code)

    # -- routes -----------------------------------------------------------
    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        b = self.bridge
        if u.path in ("/", "/index.html"):
            return self._static("index.html")
        if u.path.startswith("/static/"):
            return self._static(u.path[len("/static/"):])
        if u.path == "/api/state":
            return self._json(b.state())
        if u.path == "/api/ports":
            return self._json(b.ports())
        if u.path == "/api/screen.png":
            orient = "land" if q.get("orient", ["port"])[0] == "land" else "port"
            with b.state_lock:
                data = b.png[orient]
                seq = b.seq
            if data is None:
                return self._json({"ok": False, "error": "no frame yet"}, 404)
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Frame-Seq", str(seq))
            self.end_headers()
            self.wfile.write(data)
            return
        if u.path == "/api/events":
            return self._sse()
        self._json({"ok": False, "error": "not found"}, 404)

    def do_POST(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        b = self.bridge
        if u.path == "/api/audio":
            try:
                n = int(self.headers.get("Content-Length") or 0)
                if n <= 0 or n > 16000 * 8 * 2:
                    return self._json({"ok": False, "error": "bad pcm length"}, 400)
                pcm = self.rfile.read(n)
                return self._json(b.audio(pcm, int(q.get("followMs", ["20000"])[0])))
            except Exception as exc:
                return self._fail(exc, 409)
        try:
            body = self._body()
        except Exception as exc:
            return self._fail(exc, 400)
        try:
            if u.path == "/api/text":
                return self._json(b.text(str(body.get("text", "")), int(body.get("followMs", 20000))))
            if u.path == "/api/key":
                return self._json(
                    b.press(
                        str(body.get("key", "")),
                        int(body.get("holdMs") or 0),
                        int(body.get("settleMs", 450)),
                        bool(body.get("shot", True)),
                        int(body.get("followMs", 0)),
                    )
                )
            if u.path == "/api/shot":
                seq = b.capture()
                return self._json({"ok": True, "seq": seq, "crcOk": b.crc_ok,
                                   "crc": f"{b.frame_crc:08X}" if b.frame_crc is not None else None,
                                   "captureMs": b.capture_ms})
            if u.path == "/api/info":
                return self._json({"ok": True, "info": b.info()})
            if u.path == "/api/reconnect":
                return self._json(b.reconnect())
            if u.path == "/api/connect":
                return self._json(b.connect_port(str(body.get("port", ""))))
            if u.path == "/api/debug":
                return self._json(b.debug(str(body.get("cmd", ""))))
        except Exception as exc:
            return self._fail(exc, 409)
        self._json({"ok": False, "error": "not found"}, 404)

    def _static(self, name: str):
        path = (STATIC / name).resolve()
        if not path.is_file() or STATIC not in path.parents:
            return self._json({"ok": False, "error": "not found"}, 404)
        types = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml", ".png": "image/png"}
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", types.get(path.suffix, "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    def _sse(self):
        b = self.bridge
        q = b.subscribe()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        try:
            with b.state_lock:
                backlog = list(b.lines[-200:])
            st = b.state()
            self._sse_send("backlog", backlog)
            self._sse_send("status", {"connected": st["connected"], "port": st["port"], "error": ""})
            if st["hasScreen"]:
                self._sse_send("screen", {"seq": st["seq"], "crcOk": st["crcOk"], "ms": st["captureMs"]})
            while True:
                try:
                    event, data = q.get(timeout=15)
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
                    continue
                self._sse_send(event, data)
        except Exception:
            pass
        finally:
            b.unsubscribe(q)

    def _sse_send(self, event: str, data):
        payload = json.dumps(data, ensure_ascii=False)
        self.wfile.write(f"event: {event}\ndata: {payload}\n\n".encode())
        self.wfile.flush()


def _default_port() -> str:
    # Espressif native USB CDC first (VID 0x303A), else the first serial port
    try:
        ports = list_ports.comports()
        for p in ports:
            if p.vid == 0x303A:
                return p.device
        if ports:
            return ports[0].device
    except Exception:
        pass
    return "COM3" if os.name == "nt" else "/dev/ttyACM0"


def _lan_ip() -> str:
    """Best-effort LAN IPv4 address of this machine.

    A UDP connect() sends no packets; it only asks the kernel which source
    address the default route would use. That is the address a phone on the
    same Wi-Fi needs. getaddrinfo(hostname) is the fallback for a machine with
    no default route (isolated LAN); loopback is the last resort.
    """
    candidates = []
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            candidates.append(s.getsockname()[0])
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            candidates.append(info[4][0])
    except OSError:
        pass
    for ip in candidates:
        if ip and ip != "0.0.0.0" and not ip.startswith("127."):
            return ip
    return "127.0.0.1"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="auto", help="reader serial device (auto = detect Espressif CDC)")
    ap.add_argument("--http", type=int, default=8090, help="HTTP listen port")
    ap.add_argument(
        "--host",
        default="auto",
        help="HTTP bind address (auto = all interfaces and print the LAN URL; "
        "0.0.0.0 = all interfaces; 127.0.0.1 = local only)",
    )
    ap.add_argument("--open", action="store_true", help="open the console page in the default browser")
    args = ap.parse_args()
    port = args.port if args.port != "auto" else _default_port()
    if args.host == "auto":
        bind_host = "0.0.0.0"
        url_host = _lan_ip()
    elif args.host in ("0.0.0.0", "::"):
        bind_host = args.host
        url_host = _lan_ip()
    else:
        bind_host = args.host
        url_host = args.host
    Handler.bridge = Bridge(port)
    srv = ThreadingHTTPServer((bind_host, args.http), Handler)
    srv.daemon_threads = True
    if bind_host in ("0.0.0.0", "::"):
        scope = " (LAN)"
    elif bind_host in ("127.0.0.1", "::1", "localhost"):
        scope = " (local only)"
    else:
        scope = ""
    print(
        f"[webremote] serial={port} http=http://{url_host}:{args.http} "
        f"bind={bind_host}{scope}",
        flush=True,
    )
    if args.open:
        import threading
        import webbrowser

        threading.Timer(0.8, webbrowser.open, (f"http://{url_host}:{args.http}",)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()