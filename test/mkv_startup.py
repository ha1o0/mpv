#!/usr/bin/env python3
"""Exercise MKV header policy against an actual libmpv, using only localhost.

  python test/mkv_startup.py --libmpv /path/to/libmpv.so
  python test/mkv_startup.py --libmpv C:/path/libmpv-2.dll --baseline

No FFmpeg, external media, user configuration, or installed-player writes are
needed. The generated four-second file has PNG video, PCM audio, two UTF-8
subtitles, two chapters, and tail Cues/Tags beyond a 2 MiB Void. Each case is isolated in
a child process with a hard timeout. This tests demuxing, metadata, selection,
and seek completion, not GPU rendering, online latency, or subtitle pixels.
On-demand Cues checks use native header-seek and parse logs: a new HTTP request
is not required when the bytes are already in a stream buffer.
"""

import argparse
import ctypes as C
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit


def element(tag, data):
    tag = tag.to_bytes((tag.bit_length() + 7) // 8, "big")
    width = next(n for n in range(1, 9) if len(data) < (1 << (7 * n)) - 1)
    size = ((1 << (7 * width)) | len(data)).to_bytes(width, "big")
    return tag + size + data


def uint(tag, value, width=None):
    width = width or max(1, (value.bit_length() + 7) // 8)
    return element(tag, value.to_bytes(width, "big"))


def string(tag, value):
    return element(tag, value.encode("utf-8"))


def fixture():
    def png_chunk(kind, data):
        return (len(data).to_bytes(4, "big") + kind + data
                + (zlib.crc32(kind + data) & 0xffffffff).to_bytes(4, "big"))

    png = (b"\x89PNG\r\n\x1a\n"
           + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
           + png_chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00"))
           + png_chunk(b"IEND", b""))
    header = element(0x1A45DFA3, b"".join([
        uint(0x4286, 1), uint(0x42F7, 1), uint(0x42F2, 4), uint(0x42F3, 8),
        string(0x4282, "matroska"), uint(0x4287, 4), uint(0x4285, 2)]))
    info = element(0x1549A966, uint(0x2AD7B1, 1000000)
                   + element(0x4489, struct.pack(">d", 4000.0))
                   + string(0x4D80, "mpv-test") + string(0x5741, "mpv-test"))
    audio = element(0xAE, uint(0xD7, 1) + uint(0x73C5, 1) + uint(0x83, 2)
                    + string(0x86, "A_PCM/INT/LIT") + uint(0x23E383, 100000000)
                    + element(0xE1, element(0xB5, struct.pack(">d", 8000.0))
                              + uint(0x9F, 1) + uint(0x6264, 16)))
    # A selected video stream matters here: audio-only track selection may
    # legitimately issue an initial refresh seek and consume deferred Cues.
    video = element(0xAE, uint(0xD7, 4) + uint(0x73C5, 4) + uint(0x83, 1)
                    + string(0x86, "V_PNG") + uint(0x23E383, 100000000)
                    + element(0xE0, uint(0xB0, 1) + uint(0xBA, 1)))
    subtitles = b"".join(element(0xAE, uint(0xD7, n) + uint(0x73C5, n)
                            + uint(0x83, 17) + uint(0x88, 0)
                            + string(0x86, "S_TEXT/UTF8")
                            + string(0x536E, title) + string(0x22B59C, lang))
                         for n, title, lang in [(2, "Alpha", "eng"), (3, "Beta", "chi")])
    tracks = element(0x1654AE6B, video + audio + subtitles)
    chapters = element(0x1043A770, element(0x45B9, b"".join(
        element(0xB6, uint(0x73C4, n + 1) + uint(0x91, n * 2000000000)
                + element(0x80, string(0x85, title) + string(0x437C, "eng")))
        for n, title in enumerate(["Opening", "Later"]))))
    clusters = []
    for second in range(4):
        blocks = b"".join(element(0xA3, b"\x84" + struct.pack(">h", ms) + b"\x80" + png)
                          + element(0xA3, b"\x81" + struct.pack(">h", ms)
                                    + b"\x80" + bytes(1600))
                          for ms in range(0, 1000, 100))
        blocks += b"".join(element(0xA0,
                                  element(0xA1, bytes([0x80 | n, 0, 0, 0])
                                          + f"Subtitle {n} at {second}".encode())
                                  + uint(0x9B, 1000)) for n in [2, 3])
        clusters.append(element(0x1F43B675, uint(0xE7, second * 1000) + blocks))
    padding = element(0xEC, bytes(2 * 1024 * 1024))

    def seekhead(cues, tags):
        return element(0x114D9B74, b"".join(
            element(0x4DBB, element(0x53AB, tag.to_bytes(4, "big"))
                    + uint(0x53AC, pos, 8))
            for tag, pos in [(0x1C53BB6B, cues), (0x1254C367, tags)]))

    prefix = seekhead(0, 0) + info + tracks + chapters
    positions = [len(prefix)]
    for n in range(1, 4):
        positions.append(positions[-1] + len(clusters[n - 1])
                         + (len(padding) if n == 1 else 0))
    content = clusters[0] + padding + b"".join(clusters[1:])
    cues = element(0x1C53BB6B, b"".join(
        element(0xBB, uint(0xB3, n * 1000)
                + element(0xB7, uint(0xF7, 4) + uint(0xF1, pos)))
        for n, pos in enumerate(positions)))
    tags = element(0x1254C367, element(0x7373, element(0x63C0, b"")
                   + element(0x67C8, string(0x45A3, "COMMENT")
                             + string(0x4487, "MKV startup fixture"))))
    cue_pos = len(prefix) + len(content)
    body = seekhead(cue_pos, cue_pos + len(cues)) + info + tracks + chapters + content + cues + tags
    segment = element(0x18538067, body)
    base = len(header) + len(segment) - len(body)
    return header + segment, base + cue_pos, base + cue_pos + len(cues)


class LocalMedia:
    def __init__(self):
        self.data, self.cues, self.tags = fixture()
        self.requests = []
        self.lock = threading.Lock()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def handle(self):
                try:
                    super().handle()
                except (ConnectionResetError, ConnectionAbortedError):
                    pass  # Normal when libmpv cancels a keep-alive connection.

            def do_GET(self):
                if urlsplit(self.path).path != "/fixture.mkv":
                    self.send_error(404)
                    return
                match = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
                start = int(match[1]) if match else 0
                end = min(int(match[2]) if match and match[2] else len(owner.data) - 1,
                          len(owner.data) - 1)
                if start > end:
                    self.send_error(416)
                    return
                with owner.lock:
                    owner.requests.append(start)
                try:
                    self.send_response(206 if match else 200)
                    self.send_header("Content-Length", str(end - start + 1))
                    self.send_header("Content-Type", "video/x-matroska")
                    self.send_header("Accept-Ranges", "bytes")
                    if match:
                        self.send_header("Content-Range", f"bytes {start}-{end}/{len(owner.data)}")
                    self.end_headers()
                    for at in range(start, end + 1, 16384):
                        self.wfile.write(owner.data[at:min(at + 16384, end + 1)])
                        self.wfile.flush()
                        time.sleep(0.001)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/fixture.mkv"

    def snapshot(self):
        with self.lock:
            return self.requests.copy()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


class Event(C.Structure):
    _fields_ = [("id", C.c_int), ("error", C.c_int), ("userdata", C.c_uint64), ("data", C.c_void_p)]


class Log(C.Structure):
    _fields_ = [("prefix", C.c_char_p), ("level", C.c_char_p), ("text", C.c_char_p), ("number", C.c_int)]


class Player:
    def __init__(self, path, policy):
        self.dll = C.CDLL(str(path))
        signatures = {
            "mpv_create": (C.c_void_p, []),
            "mpv_initialize": (C.c_int, [C.c_void_p]),
            "mpv_set_option_string": (C.c_int, [C.c_void_p, C.c_char_p, C.c_char_p]),
            "mpv_set_property_string": (C.c_int, [C.c_void_p, C.c_char_p, C.c_char_p]),
            "mpv_get_property_string": (C.c_void_p, [C.c_void_p, C.c_char_p]),
            "mpv_command": (C.c_int, [C.c_void_p, C.POINTER(C.c_char_p)]),
            "mpv_wait_event": (C.POINTER(Event), [C.c_void_p, C.c_double]),
            "mpv_request_log_messages": (C.c_int, [C.c_void_p, C.c_char_p]),
            "mpv_free": (None, [C.c_void_p]),
            "mpv_terminate_destroy": (None, [C.c_void_p]),
        }
        for name, (restype, argtypes) in signatures.items():
            fn = getattr(self.dll, name)
            fn.restype, fn.argtypes = restype, argtypes
        self.handle = self.dll.mpv_create()
        if not self.handle:
            raise RuntimeError("mpv_create failed")
        self.logs = []
        self.pending = []
        options = {"config": "no", "terminal": "no", "load-scripts": "no", "ytdl": "no",
                   "vo": "null", "ao": "null", "hwdec": "no", "idle": "yes", "keep-open": "yes",
                   "pause": "yes", "sid": "2", "sub-auto": "no", "cache": "no",
                   "demuxer-thread": "no", "demuxer-readahead-secs": "0",
                   "stream-buffer-size": "64KiB", "http-proxy": "", "network-timeout": "5"}
        options.update(policy)
        try:
            for key, value in options.items():
                result = self.dll.mpv_set_option_string(self.handle, key.encode(), value.encode())
                # These controls do not exist when the corresponding scripting
                # support is not compiled in. All policy options stay required.
                if result == -5 and key in ("load-scripts", "ytdl"):
                    continue  # MPV_ERROR_OPTION_NOT_FOUND
                self.check(result, key)
            self.check(self.dll.mpv_initialize(self.handle), "initialize")
            self.check(self.dll.mpv_request_log_messages(self.handle, b"debug"), "log subscription")
        except BaseException:
            self.close()
            raise

    @staticmethod
    def check(code, action):
        if code < 0:
            raise RuntimeError(f"{action}: mpv error {code}")

    def command(self, *args):
        args = (C.c_char_p * (len(args) + 1))(*(arg.encode() for arg in args), None)
        self.check(self.dll.mpv_command(self.handle, args), "command")

    def set(self, key, value):
        self.check(self.dll.mpv_set_property_string(self.handle, key.encode(), value.encode()), key)

    def get(self, key):
        value = self.dll.mpv_get_property_string(self.handle, key.encode())
        if not value:
            return None
        try:
            return C.string_at(value).decode("utf-8", "replace")
        finally:
            self.dll.mpv_free(value)

    def raw_event(self, wait=0.02):
        event = self.dll.mpv_wait_event(self.handle, wait).contents
        if event.id == 2 and event.data:
            log = C.cast(event.data, C.POINTER(Log)).contents
            if log.prefix == b"mkv":
                self.logs.append(log.text.decode("utf-8", "replace").strip())
        if event.id == 24:
            raise RuntimeError("event queue overflow")
        return event.id

    def event(self, wait=0.02):
        return self.pending.pop(0) if self.pending else self.raw_event(wait)

    def drain_logs(self):
        # mpv delivers queued lifecycle events before its separate log buffer.
        # Drain that buffer at phase boundaries without losing an early restart.
        for _ in range(10000):
            event = self.raw_event(0)
            if not event:
                return
            if event != 2:
                self.pending.append(event)
        raise RuntimeError("log drain did not become idle")

    def wait(self, wanted, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.event() == wanted:
                self.drain_logs()
                return
        raise RuntimeError(f"event {wanted} timeout; last MKV messages: {self.logs[-5:]}")

    def load(self, url, local=None):
        self.command("stop")
        while self.event(0):
            pass
        self.logs.clear()
        self.command("loadfile", url, "replace", "-1", local or "")
        self.wait(8)  # MPV_EVENT_FILE_LOADED

    def close(self):
        if self.handle:
            self.dll.mpv_terminate_destroy(self.handle)
            self.handle = None


DEFER = "demuxer-mkv-defer-cues"
SKIP = "demuxer-mkv-skip-tags"


def require(condition, detail):
    if not condition:
        raise AssertionError(detail)


def inspect(player, skip):
    require(player.get("current-demuxer") == "mkv", "wrong demuxer")
    chapters = json.loads(player.get("chapter-list") or "[]")
    tracks = json.loads(player.get("track-list") or "[]")
    metadata = json.loads(player.get("metadata") or "{}")
    require([c.get("title") for c in chapters] == ["Opening", "Later"], "chapters missing")
    require([c.get("time") for c in chapters] == [0, 2], "chapter times changed")
    subs = [t for t in tracks if t.get("type") == "sub"]
    require(len(tracks) == 4 and len(subs) == 2
            and sum(t.get("type") == "video" for t in tracks) == 1
            and sum(t.get("type") == "audio" for t in tracks) == 1,
            "video/audio/subtitle tracks missing")
    require([t.get("title") for t in subs] == ["Alpha", "Beta"], "subtitle titles changed")
    require(player.get("sid") == "2", "initial subtitle selection changed")
    has_tag = any(k.lower() == "comment" and v == "MKV startup fixture" for k, v in metadata.items())
    require(has_tag != skip, f"unexpected COMMENT metadata: skip={skip}, present={has_tag}")


def run_case(path, case):
    policies = {"default": {}, "no": {DEFER: "no", SKIP: "no"},
                "defer": {DEFER: "yes", SKIP: "no"}, "skip": {DEFER: "no", SKIP: "yes"},
                "both": {DEFER: "yes", SKIP: "yes"},
                "start": {DEFER: "yes", SKIP: "yes", "start": "2.5"},
                "local": {DEFER: "no", SKIP: "no"}}
    media = LocalMedia()
    player = None
    try:
        player = Player(path, policies[case])
        local = f"{DEFER}=yes,{SKIP}=yes" if case == "local" else None
        player.load(media.url, local)
        initial = media.snapshot()
        skip = case in ("skip", "both", "start", "local")
        inspect(player, skip)
        tail = any(offset >= media.cues for offset in initial)
        parsed_cues = any("Parsing cues" in line for line in player.logs)
        if case in ("default", "no"):
            require(tail, "default fixture did not exercise a tail header request")
            require(parsed_cues, "default policy did not parse Cues before file-loaded")
        if case == "defer":
            require(not parsed_cues, "defer-only still parsed Cues before file-loaded")
            require(media.tags in initial, "defer-only did not request tail Tags")
        if case in ("skip", "both", "local"):
            require(not tail, f"tail Range requested before file-loaded: {initial}")
            require(not parsed_cues, "Cues were parsed before file-loaded")
            require(any("Deferring reading cues" in line for line in player.logs), "no deferred-cues log")
        player.wait(21)  # paused initial audio output is ready
        if case == "start":
            require(float(player.get("time-pos") or -1) >= 2.4, "nonzero start did not seek")
        before_seek = len(media.snapshot())
        before_seek_logs = len(player.logs)
        player.command("seek", "3", "absolute+exact")
        player.wait(21)
        seek_ranges = media.snapshot()[before_seek:]
        require(abs(float(player.get("time-pos") or -1) - 3) < 0.15, "seek did not reach 3 seconds")
        if case in ("both", "local"):
            # Reading the index may hit the stream buffer. A new HTTP Range
            # would be unnecessary in that case; require the actual MKV read.
            cue_seek = f"Seeking to {media.cues} to read header element 0x1c53bb6b."
            require(cue_seek in player.logs[before_seek_logs:],
                    "deferred Cues header was not read on seek")
            require(any("Parsing cues" in line for line in player.logs[before_seek_logs:]),
                    "deferred Cues were not parsed on seek")
        player.set("pause", "no")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and float(player.get("time-pos") or 0) < 3.15:
            player.event()
        require(float(player.get("time-pos") or 0) >= 3.15, "playback did not resume after seek")
        player.set("pause", "yes")
        inspect(player, skip)
        # Switching subtitles can legitimately seek. Do this only after the
        # startup and first on-demand index checks so it cannot mask either.
        player.set("sid", "1")
        require(player.get("sid") == "1", "subtitle switch failed")
        player.set("sid", "2")
        if case == "local":
            begin = len(media.snapshot())
            player.load(media.url)
            inspect(player, False)
            require(player.get("options/" + DEFER) == "no" and player.get("options/" + SKIP) == "no",
                    "file-local policy leaked to next file")
            require(any(offset >= media.cues for offset in media.snapshot()[begin:]),
                    "next file did not restore normal tail header reads")
        return {"case": case, "pass": True, "mpv": player.get("mpv-version"),
                "fileBytes": len(media.data), "cuesOffset": media.cues,
                "startupRanges": initial, "seekRanges": seek_ranges,
                "allRanges": media.snapshot()}
    finally:
        if player:
            player.close()
        media.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--libmpv", required=True, type=Path)
    parser.add_argument("--baseline", action="store_true", help="test only existing default behavior")
    parser.add_argument("--case", choices=["default", "no", "defer", "skip", "both", "start", "local"],
                        help=argparse.SUPPRESS)
    args = parser.parse_args()
    library = args.libmpv.resolve(strict=True)
    if args.case:
        print(json.dumps(run_case(library, args.case)))
        return
    cases = ["default"] if args.baseline else ["default", "no", "defer", "skip", "both", "start", "local"]
    env = {k: v for k, v in os.environ.items() if k.lower() not in
           ("http_proxy", "https_proxy", "all_proxy", "no_proxy")}
    env["NO_PROXY"] = "127.0.0.1,localhost"
    for case in cases:
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--libmpv", str(library),
                                 "--case", case], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=40, env=env)
        if result.returncode:
            raise RuntimeError(f"case {case} failed ({result.returncode})\n{result.stderr[-4000:]}")
        print(result.stdout.strip(), flush=True)
    print("Baseline fixture validated; new options were not tested." if args.baseline else "All MKV startup cases passed.")


if __name__ == "__main__":
    main()
