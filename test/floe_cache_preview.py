#!/usr/bin/env python3
"""Check HEVC/MKV cached preview timestamps and open GOPs with real libmpv.

  python test/floe_cache_preview.py --library /path/to/libmpv.so
  python test/floe_cache_preview.py --library /path/to/libmpv.dylib
  python test/floe_cache_preview.py --library C:/path/to/libmpv-2.dll

The small synthetic fixture has SDR HEVC with four B frames/open GOPs and AAC.
The test uses private players and temporary outputs, with no network access.

Recreate test/samples/cache_preview_hevc.mkv with FFmpeg/libx265:

  ffmpeg -f lavfi -i testsrc2=size=96x64:rate=25:duration=4 \\
    -f lavfi -i sine=frequency=440:sample_rate=8000:duration=4 \\
    -map 0:v -map 1:a -c:v libx265 -preset fast \\
    -x265-params log-level=error:keyint=25:min-keyint=25:scenecut=0:bframes=4:open-gop=1 \\
    -pix_fmt yuv420p -c:a aac -b:a 12k -threads 1 -shortest \\
    test/samples/cache_preview_hevc.mkv

This regression is expected to fail with a libmpv built before the snapshot fix.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import struct
import tempfile
import time
import zlib


class Event(C.Structure):
    _fields_ = [('id', C.c_int), ('error', C.c_int),
                ('reply', C.c_uint64), ('data', C.c_void_p)]


class Player:
    def __init__(self, library, **options):
        self.dll_directory = os.add_dll_directory(str(library.parent)) if os.name == 'nt' else None
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
        for name, (arguments, result) in signatures.items():
            getattr(self.lib, name).argtypes = arguments
            getattr(self.lib, name).restype = result
        self.handle = self.lib.mpv_create()
        assert self.handle
        defaults = {'config': 'no', 'load-scripts': 'no', 'terminal': 'no',
                    'vo': 'null', 'ao': 'null', 'hwdec': 'no', 'pause': 'yes',
                    'sub': 'no', 'cache': 'yes', 'demuxer-readahead-secs': '20',
                    'demuxer-max-bytes': '8388608', 'demuxer-max-back-bytes': '8388608'}
        try:
            for name, value in {**defaults, **options}.items():
                assert self.lib.mpv_set_option_string(self.handle, name.encode(), value.encode()) >= 0, name
            assert self.lib.mpv_initialize(self.handle) >= 0
        except BaseException:
            self.close()
            raise

    def command(self, *arguments):
        args = (C.c_char_p * (len(arguments) + 1))(
            *(argument.encode() for argument in arguments), None)
        result = self.lib.mpv_command(self.handle, args)
        assert result >= 0, (arguments[0], result)

    def get(self, name):
        pointer = self.lib.mpv_get_property_string(self.handle, name.encode())
        if not pointer:
            return None
        try:
            text = C.string_at(pointer).decode('utf-8')
            try:
                return json.loads(text)
            except ValueError:
                return text
        finally:
            self.lib.mpv_free(pointer)

    def frame(self, *command):
        while self.lib.mpv_wait_event(self.handle, 0).contents.id:
            pass
        self.command(*command)
        loaded = command[0] != 'loadfile'
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            event = self.lib.mpv_wait_event(self.handle, 0.05).contents.id
            if event == 8:
                loaded = True
            if event == 21 and loaded and isinstance(self.get('time-pos'), (int, float)):
                return
        raise AssertionError(('no playback restart', command[0]))

    def wait_cached(self):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.lib.mpv_wait_event(self.handle, 0.05)
            state = self.get('demuxer-cache-state')
            if state and state.get('eof-cached'):
                return
        raise AssertionError('fixture was not fully cached')

    def state(self):
        memory = self.get('demuxer-cache-state')
        return (self.get('time-pos'), self.get('vid'), self.get('aid'),
                memory['seekable-ranges'], memory['debug-low-level-seeks'],
                memory['debug-byte-level-seeks'])

    def close(self):
        self.lib.mpv_terminate_destroy(self.handle)
        if self.dll_directory:
            self.dll_directory.close()


def pixels(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset, header, compressed = 8, None, bytearray()
    while offset + 12 <= len(data):
        length = struct.unpack_from('>I', data, offset)[0]
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        if kind == b'IHDR':
            header = body
        if kind == b'IDAT':
            compressed.extend(body)
        offset += length + 12
    assert header and compressed
    return header, zlib.decompress(compressed)


def verify(library, fixture, root):
    reference = Player(library, cache='no')
    try:
        reference.frame('loadfile', str(fixture))
        for on_disk in ['no', 'yes']:
            primary = Player(library, **{'cache-on-disk': on_disk})
            try:
                primary.frame('loadfile', str(fixture))
                primary.wait_cached()
                before = primary.state()
                for target in [0.125, 0.875, 1.0, 1.25, 2.75, 3.25]:
                    clip = root / f'{on_disk}-{target}.nut'
                    primary.command('floe-cache-snapshot', str(target), str(clip))
                    timing = json.loads(Path(str(clip) + '.json').read_text())
                    preview = Player(library, cache='no', audio='no',
                                     **{'start': str(timing['position']), 'access-references': 'no'})
                    try:
                        preview.frame('loadfile', str(clip))
                        preview.frame('seek', str(timing['position']), 'absolute+exact')
                        reference.frame('seek', str(target), 'absolute+exact')
                        actual, expected = root / 'actual.png', root / 'expected.png'
                        preview.command('screenshot-to-file', str(actual), 'video')
                        reference.command('screenshot-to-file', str(expected), 'video')
                        assert pixels(actual) == pixels(expected), (on_disk, target, timing)
                        assert primary.state() == before, 'preview changed playback/cache readers'
                    finally:
                        preview.close()
                    print(json.dumps({'cache_on_disk': on_disk, 'target': target,
                                      'pixels_match': True, 'playback_unchanged': True}), flush=True)
            finally:
                primary.close()
    finally:
        reference.close()


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--fixture', type=Path,
                        default=Path(__file__).parent / 'samples/cache_preview_hevc.mkv')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mpv-cache-preview-') as directory:
        verify(args.library.resolve(), args.fixture.resolve(), Path(directory))


if __name__ == '__main__':
    main()
