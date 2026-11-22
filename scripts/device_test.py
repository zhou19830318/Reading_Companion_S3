#!/usr/bin/env python3
"""Serial device-test driver for the read-aloud mode verification.

Speaks the firmware's CMD: protocol (src/main.cpp): synthetic key presses,
framebuffer screenshots, relay config queries. Steps are given as a comma
separated DSL so a test run can be orchestrated from the shell:

  sleep:3                 wait 3 s
  cmd:CLOUD               send "CMD:CLOUD"
  key:ok                  send "CMD:KEY ok"
  keyhold:back:1200       send "CMD:KEYHOLD back 1200"
  wait:read-aloud start:30   wait up to 30 s for a log line matching the regex
  shot:menu               capture the framebuffer to <out>/menu.png

Every serial line is appended to <out>/serial.log. Screenshots are validated
against the FNV-1a CRC the firmware prints before the dump; a mismatch retries
once. Exit code is non-zero when a wait times out or a capture stays corrupt.
"""

import argparse
import re
import sys
import threading
import time
from pathlib import Path

import serial
from PIL import Image

SCREEN_W, SCREEN_H = 800, 480


class Device:
    def __init__(self, port: str, out: Path):
        self.out = out
        out.mkdir(parents=True, exist_ok=True)
        self.log = open(out / "serial.log", "ab", buffering=0)
        self.ser = serial.Serial(port, 115200, timeout=0.1)
        self.lines: list[str] = []
        self.lock = threading.Lock()
        self.new_line = threading.Condition(self.lock)
        self.shot_event = threading.Event()
        self.shot_crc_ok = False
        self.shot_error = ""
        self._stop = False
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        buf = b""
        expecting = 0
        shot = b""
        want_crc = None
        while not self._stop:
            data = self.ser.read(4096)
            if not data:
                continue
            buf += data
            while True:
                if expecting:
                    need = expecting - len(shot)
                    if len(buf) < need:
                        break
                    shot += buf[:need]
                    buf = buf[need:]
                    expecting = 0
                    crc = 2166136261
                    for b in shot:
                        crc = ((crc ^ b) * 16777619) & 0xFFFFFFFF
                    self.shot_crc_ok = want_crc is None or crc == want_crc
                    if self.shot_crc_ok:
                        img = Image.frombytes("1", (SCREEN_W, SCREEN_H), shot)
                        img.save(self._shot_path)
                    else:
                        self.shot_error = f"CRC mismatch: got {crc:08X} want {want_crc:08X}"
                    self.shot_event.set()
                    continue
                idx = buf.find(b"\n")
                if idx < 0:
                    break
                raw, buf = buf[:idx], buf[idx + 1:]
                line = raw.decode("utf-8", "replace").rstrip("\r")
                if line.startswith("SCREENSHOT_START:"):
                    expecting = int(line.split(":")[1])
                    shot = b""
                    continue
                if line.startswith("FBPTR:") and "CRC:" in line:
                    want_crc = int(line.split("CRC:")[1].strip(), 16)
                    continue
                if line in ("SCREENSHOT_END",) or line.startswith("SCREENSHOT_SENT:"):
                    continue
                with self.new_line:
                    self.lines.append(line)
                    self.new_line.notify_all()
                self.log.write((line + "\n").encode())
                print(f"[dev] {line}", flush=True)

    def send(self, cmd: str):
        shown = re.sub(r"(RELAY\s+\S+\|)\S+", r"\1***", cmd)
        print(f"[cmd] CMD:{shown}", flush=True)
        self.ser.write(f"CMD:{cmd}\n".encode())
        self.ser.flush()

    def wait(self, pattern: str, timeout: float) -> str:
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        with self.new_line:
            cursor = len(self.lines)
            while True:
                while cursor < len(self.lines):
                    line = self.lines[cursor]
                    cursor += 1
                    if rx.search(line):
                        return line
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise TimeoutError(f"no line matched {pattern!r} in {timeout}s")
                self.new_line.wait(min(remaining, 0.5))

    def screenshot(self, name: str, retries: int = 2):
        self._shot_path = self.out / f"{name}.png"
        for attempt in range(retries + 1):
            self.shot_event.clear()
            self.shot_error = ""
            self.send("SCREENSHOT")
            if not self.shot_event.wait(30):
                raise TimeoutError(f"screenshot {name} did not complete")
            if self.shot_crc_ok:
                print(f"[shot] {self._shot_path}", flush=True)
                return
            print(f"[shot] {self.shot_error}, retry {attempt + 1}", flush=True)
            time.sleep(1)
        raise RuntimeError(f"screenshot {name} corrupt: {self.shot_error}")

    def close(self):
        self._stop = True
        self.thread.join(timeout=2)
        self.ser.close()
        self.log.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--out", default="docs/screenshots/read-aloud")
    ap.add_argument("--steps", required=True)
    args = ap.parse_args()

    dev = Device(args.port, Path(args.out))
    failed = False
    try:
        for step in args.steps.split(","):
            step = step.strip()
            if not step:
                continue
            parts = step.split(":")
            kind = parts[0]
            try:
                if kind == "sleep":
                    time.sleep(float(parts[1]))
                elif kind == "cmd":
                    dev.send(":".join(parts[1:]))
                elif kind == "key":
                    dev.send(f"KEY {parts[1]}")
                elif kind == "keyhold":
                    dev.send(f"KEYHOLD {parts[1]} {parts[2] if len(parts) > 2 else 1000}")
                elif kind == "wait":
                    timeout = float(parts[-1]) if len(parts) > 2 else 30.0
                    pattern = ":".join(parts[1:-1]) if len(parts) > 2 else parts[1]
                    line = dev.wait(pattern, timeout)
                    print(f"[ok] matched: {line}", flush=True)
                elif kind == "shot":
                    dev.screenshot(parts[1])
                else:
                    raise ValueError(f"unknown step {step!r}")
            except (TimeoutError, RuntimeError) as e:
                print(f"[FAIL] {step}: {e}", flush=True)
                failed = True
                break
    finally:
        dev.close()
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()