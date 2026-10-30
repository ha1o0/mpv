#!/usr/bin/env python3
"""Verify real demux timestamps and persistent bytes during resume and seek.

Uses a local Range server and an FFmpeg-generated VBR fixture. --report exports
the actual property snapshots for consumers to test their timeline renderer.
"""
import argparse
import ctypes as C
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from floe_cache_http import Handler, Event, SOURCE


class Player:
    def __init__(self, library, options):
        self.dll_directory = (os.add_dll_directory(str(library.parent))
                              if os.name == 'nt' else None)
        self.lib = C.CDLL(str(library))
        signatures = {
            'mpv_create': ([], C.c_void_p),
            'mpv_initialize': ([C.c_void_p], C.c_int),
            'mpv_set_option_string': ([C.c_void_p, C.c_char_p, C.c_char_p], C.c_int),
            'mpv_get_property_string': ([C.c_void_p, C.c_char_p], C.c_void_p),
            'mpv_free': ([C.c_void_p], None),
            'mpv_command': ([C.c_void_p, C.POINTER(C.c_char_p)], C.c_int),
            'mpv_wait_event': ([C.c_void_p, C.c_double], C.POINTER(Event)),
            'mpv_terminate_destroy': ([C.c_void_p], None),
        }
        for name, (args, result) in signatures.items():
            getattr(self.lib, name).argtypes = args
            getattr(self.lib, name).restype = result
        self.handle = self.lib.mpv_create()
        defaults = {'config': 'no', 'vo': 'null', 'ao': 'null', 'terminal': 'no',
                    'idle': 'yes', 'pause': 'yes', 'cache': 'yes',
                    'cache-secs': '8', 'demuxer-readahead-secs': '8',
                    'demuxer-max-bytes': '1048576', 'network-timeout': '2',
                    'curl-max-request-size': '262144', 'stream-buffer-size': '4096'}
        try:
            for name, value in {**defaults, **options}.items():
                assert self.lib.mpv_set_option_string(
                    self.handle, name.encode(), value.encode()) >= 0, name
            assert self.lib.mpv_initialize(self.handle) >= 0
        except BaseException:
            self.close()
            raise

    def command(self, *args):
        command = (C.c_char_p * (len(args) + 1))(
            *(arg.encode() for arg in args), None)
        assert self.lib.mpv_command(self.handle, command) >= 0, args

    def get(self, name):
        pointer = self.lib.mpv_get_property_string(self.handle, name.encode())
        if not pointer:
            return None
        try:
            value = C.string_at(pointer).decode('utf8')
            try:
                return json.loads(value)
            except ValueError:
                return value
        finally:
            self.lib.mpv_free(pointer)

    def snapshot(self, target, cached):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            self.lib.mpv_wait_event(self.handle, 0.05)
            position = self.get('time-pos')
            mapping = self.get('demuxer-cache-time-map')
            state = self.get('floe-cache-state')
            idle = self.get('demuxer-cache-idle')
            if (isinstance(position, (int, float)) and abs(position - target) < 0.3
                    and mapping and len(mapping['tracks']) == 2
                    and all(track['ranges'] for track in mapping['tracks'])
                    and idle == 'yes' and (not cached or (state and state['ranges']))):
                # A paused low-bitrate stream must publish its short pending
                # fragment without EOF or another seek filling a 4 MiB chunk.
                if cached and not covers_time(mapping, state, target + 2):
                    continue
                return {'target': target, 'position': position, 'map': mapping,
                        'state': state, 'memory': self.get('demuxer-cache-state'),
                        'duration': self.get('duration')}
        raise AssertionError({'target': target, 'position': position,
                              'map': mapping, 'state': state, 'idle': idle})

    def close(self):
        self.lib.mpv_terminate_destroy(self.handle)
        if self.dll_directory:
            self.dll_directory.close()


def covers_time(mapping, state, target):
    ranges = sorted((r['start'], r['end']) for r in state['ranges'])
    merged = []
    for start, end in ranges:
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return all(any(t0 <= target < t1 and any(a <= b0 and b1 <= b for a, b in merged)
                   for b0, b1, t0, t1 in track['ranges']) for track in mapping['tracks'])


def verify_large_index(args, root):
    """A long packet queue must expose both sides of the old 4096-entry cap."""
    fixture = root / 'long-index.mp4'
    subprocess.run([args.ffmpeg, '-hide_banner', '-loglevel', 'error',
                    '-f', 'lavfi', '-i', 'color=c=blue:s=16x16:r=1:d=5002',
                    '-an', '-c:v', 'libx264', '-preset', 'ultrafast', '-g', '1',
                    '-threads', '1', '-movflags', '+faststart', str(fixture)],
                   check=True, timeout=60)
    player = Player(args.library.resolve(), {
        'cache-secs': '10000', 'demuxer-readahead-secs': '10000',
        'demuxer-max-bytes': str(64 * 1024 * 1024),
    })
    try:
        player.command('loadfile', str(fixture))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            player.lib.mpv_wait_event(player.handle, 0.05)
            memory = player.get('demuxer-cache-state')
            # Read the map after the EOF state, so both samples describe the
            # fully populated queue even when decoding finishes between calls.
            mapping = player.get('demuxer-cache-time-map')
            if mapping and memory and memory.get('eof'):
                break
        assert mapping and memory and memory.get('eof'), memory
        ranges = mapping['tracks'][0]['ranges']
        assert len(ranges) > 4096, len(ranges)
        assert min(r[2] for r in ranges) == 0, ranges[0]
        assert max(r[3] for r in ranges) >= 5001, ranges[-1]
        print(json.dumps({'case': 'large-index', 'ranges': len(ranges),
                          'first_time': min(r[2] for r in ranges),
                          'last_time': max(r[3] for r in ranges)}), flush=True)
    finally:
        player.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--ffmpeg', default='ffmpeg')
    parser.add_argument('--ffprobe', default='ffprobe')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--container', choices=('mp4', 'mkv'), default='mp4')
    args = parser.parse_args()
    results = []
    with tempfile.TemporaryDirectory(prefix='floe-timeline-') as directory:
        root = Path(directory)
        verify_large_index(args, root)
        fixture = root / 'vbr.mp4'
        subprocess.run([args.ffmpeg, '-hide_banner', '-loglevel', 'error',
                        '-f', 'lavfi', '-i', 'color=c=blue:s=320x180:r=25:d=20',
                        '-f', 'lavfi', '-i', 'testsrc2=s=320x180:r=25:d=40',
                        '-f', 'lavfi', '-i', 'sine=frequency=440:duration=60',
                        '-filter_complex', '[0:v][1:v]concat=n=2:v=1:a=0[v]',
                        '-map', '[v]', '-map', '2:a', '-c:v', 'libx264',
                        '-preset', 'ultrafast', '-g', '50', '-crf', '20',
                        '-threads', '1', '-c:a', 'aac', '-movflags', '+faststart',
                        str(fixture)], check=True, timeout=60)
        if args.container == 'mkv':
            remuxed = root / 'vbr.mkv'
            subprocess.run([args.ffmpeg, '-hide_banner', '-loglevel', 'error',
                            '-i', str(fixture), '-c', 'copy', str(remuxed)],
                           check=True, timeout=30)
            fixture = remuxed
        probe = json.loads(subprocess.check_output([
            args.ffprobe, '-v', 'error', '-show_packets', '-show_entries',
            'packet=stream_index,pos,pts_time,flags', '-of', 'json', str(fixture)]))
        keyframes = {(int(p['stream_index']), int(p['pos'])): float(p['pts_time'])
                     for p in probe['packets'] if 'K' in p['flags'] and 'pos' in p}
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.daemon_threads = True
        server.payload = fixture.read_bytes()
        server.offline = False
        server.kind, server.cookie, server.records = 'ordinary', False, []
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = 'http://127.0.0.1:{}/movie.mp4'.format(server.server_port)
        key = hashlib.sha256(b'timeline-vbr').hexdigest()
        base = {'floe-cache-dir': str(root / 'cache'), 'floe-cache-source': SOURCE,
                'floe-cache-url': url, 'floe-cache-key': key,
                'floe-cache-max-bytes': str(32 * 1024 * 1024), 'start': '18'}
        try:
            for label, overrides, cached in [
                ('native', {'floe-cache-enabled': 'yes', 'curl-enabled': 'yes'}, True),
                ('reopen', {'floe-cache-enabled': 'yes', 'curl-enabled': 'yes'}, True),
                ('disabled', {'floe-cache-enabled': 'no', 'curl-enabled': 'yes'}, False),
                ('ffmpeg', {'floe-cache-enabled': 'no', 'curl-enabled': 'no'}, False),
            ]:
                player = Player(args.library.resolve(), {**base, **overrides})
                try:
                    player.command('loadfile', url)
                    for target in (18, 42, 6):
                        if target != 18:
                            player.command('seek', str(target), 'absolute+exact')
                        snapshot = player.snapshot(target, cached)
                        assert snapshot['map']['path'] == url
                        verified = 0
                        mismatch = 0
                        for track in snapshot['map']['tracks']:
                            for start, end, t0, t1 in track['ranges']:
                                assert 0 <= start < end <= len(server.payload)
                                assert 0 <= t0 < t1 <= 60.1
                                pts = keyframes.get((track['id'], start))
                                if pts is None and args.container == 'mkv':
                                    # mpv's MKV demuxer reports the block data
                                    # position; libavformat includes its EBML
                                    # element header in the packet position.
                                    pts = next((value for (stream, pos), value in keyframes.items()
                                                if stream == track['id'] and abs(pos - start) <= 16), None)
                                if pts is not None:
                                    assert abs(pts - t0) < 0.05, (pts, t0)
                                    verified += 1
                                    mismatch = max(mismatch, abs(start / len(server.payload) - t0 / 60))
                        assert verified > 0
                        if target == 18:
                            assert mismatch > 0.1, 'fixture must expose byte/time mismatch'
                        snapshot.update(case=label, verified_keyframes=verified,
                                        max_byte_time_mismatch=mismatch)
                        results.append(snapshot)
                        print(json.dumps({'case': label, 'target': target,
                                          'verified_keyframes': verified,
                                          'byte_time_mismatch': round(mismatch, 3)}), flush=True)
                finally:
                    player.close()
            assert server.records and all(r['status'] < 400 for r in server.records)
            for chunk in (root / 'cache').rglob('*.chunk'):
                parts = chunk.name.split('.')
                data = chunk.read_bytes()
                start, length = int(parts[2]), int(parts[3])
                assert len(data) == length and hashlib.sha256(data).hexdigest() == parts[4]
                assert data == server.payload[start:start + length]
        finally:
            server.shutdown()
            server.server_close()
    if args.report:
        args.report.write_text(json.dumps(results, indent=2), encoding='utf8')


if __name__ == '__main__':
    main()
