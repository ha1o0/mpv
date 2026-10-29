#!/usr/bin/env python3
"""Run real libmpv cache compatibility checks on the library's target OS.

Each player runs in an isolated child process against a local HTTP fixture.
Use --fixture with an MP4 longer than four seconds, or --ffmpeg to generate it.
Cross-built Windows libraries must run on Windows with their matching DLLs.
"""

import argparse
import ctypes as C
import gzip
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time


SOURCE = '12345678-1234-1234-1234-123456789abc'
CASES = ('ordinary', 'ffmpeg', 'cookie', 'cookie-file', 'no-store-cookie',
         'no-validator-cookie', 'gzip', 'environment-proxy', 'explicit-proxy',
         'unavailable-disk', 'conditional-header', 'local-file')


class Event(C.Structure):
    _fields_ = [('event_id', C.c_int), ('error', C.c_int),
                ('reply_userdata', C.c_uint64), ('data', C.c_void_p)]


class EndFile(C.Structure):
    _fields_ = [('reason', C.c_int), ('error', C.c_int)]


class Log(C.Structure):
    _fields_ = [('prefix', C.c_char_p), ('level', C.c_char_p),
                ('text', C.c_char_p), ('log_level', C.c_int)]


def player(library):
    settings = json.load(sys.stdin)
    dll_directory = os.add_dll_directory(str(library.parent)) if os.name == 'nt' else None
    lib = C.CDLL(str(library))
    signatures = {
        'mpv_create': ([], C.c_void_p),
        'mpv_initialize': ([C.c_void_p], C.c_int),
        'mpv_set_option_string': ([C.c_void_p, C.c_char_p, C.c_char_p], C.c_int),
        'mpv_set_property_string': ([C.c_void_p, C.c_char_p, C.c_char_p], C.c_int),
        'mpv_request_log_messages': ([C.c_void_p, C.c_char_p], C.c_int),
        'mpv_get_property_string': ([C.c_void_p, C.c_char_p], C.c_void_p),
        'mpv_free': ([C.c_void_p], None),
        'mpv_command': ([C.c_void_p, C.POINTER(C.c_char_p)], C.c_int),
        'mpv_wait_event': ([C.c_void_p, C.c_double], C.POINTER(Event)),
        'mpv_terminate_destroy': ([C.c_void_p], None),
    }
    for name, (arguments, result) in signatures.items():
        getattr(lib, name).argtypes = arguments
        getattr(lib, name).restype = result
    handle = lib.mpv_create()
    result = {'loaded': False, 'end_reason': None, 'end_error': None,
              'errors': [], 'logs': [], 'cache_state': None}

    def check(code, name):
        if code < 0:
            raise RuntimeError('{} returned {}'.format(name, code))

    try:
        options = {'config': 'no', 'load-scripts': 'no', 'ytdl': 'no',
                   'vo': 'null', 'ao': 'null', 'terminal': 'no', 'untimed': 'yes',
                   'idle': 'yes', 'network-timeout': '2'}
        for name, value in options.items():
            code = lib.mpv_set_option_string(handle, name.encode(), value.encode())
            if code == -5 and name in ('load-scripts', 'ytdl'):
                continue
            check(code, name)
        check(lib.mpv_initialize(handle), 'initialize')
        lib.mpv_request_log_messages(handle, b'warn')
        for name, value in settings['properties'].items():
            check(lib.mpv_set_property_string(handle, name.encode(), value.encode()), name)
        command = (C.c_char_p * 3)(b'loadfile', settings['url'].encode('utf8'), None)
        check(lib.mpv_command(handle, command), 'loadfile')
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            event = lib.mpv_wait_event(handle, 0.05).contents
            if event.event_id == 8:
                result['loaded'] = True
                pointer = lib.mpv_get_property_string(handle, b'floe-cache-state')
                if pointer:
                    try:
                        result['cache_state'] = C.string_at(pointer).decode('utf8')
                    finally:
                        lib.mpv_free(pointer)
            elif event.event_id == 2:
                entry = C.cast(event.data, C.POINTER(Log)).contents
                result['logs'].append(entry.text.decode('utf8', errors='replace'))
            elif event.event_id == 7:
                ending = C.cast(event.data, C.POINTER(EndFile)).contents
                result['end_reason'], result['end_error'] = ending.reason, ending.error
                break
        else:
            raise RuntimeError('playback did not reach END_FILE')
    except Exception as error:
        result['errors'].append(str(error))
    finally:
        lib.mpv_terminate_destroy(handle)
        if dll_directory:
            dll_directory.close()
    result['logs'] = result['logs'][-80:]
    print(json.dumps(result), flush=True)
    return int(not result['loaded'] or result['end_reason'] != 0 or bool(result['errors']))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        start, end = 0, len(self.server.payload)
        range_header = self.headers.get('Range')
        if range_header:
            left, right = range_header[6:].split('-')
            start = int(left)
            end = min(end, int(right) + 1) if right else end
        status = 206 if range_header else 200
        cookie = self.headers.get('Cookie') or ''
        if self.server.cookie and start and 'floe_test=valid' not in cookie:
            status = 403
        if start >= end:
            status = 416
        body = self.server.payload[start:end] if status < 400 else b'fixture authentication failed'
        encoded = self.server.kind == 'gzip' and 'gzip' in self.headers.get('Accept-Encoding', '')
        if encoded:
            body = gzip.compress(body)
        self.server.records.append({'path': self.path, 'range': range_header,
                                    'cookie': cookie, 'status': status,
                                    'encoding': self.headers.get('Accept-Encoding'),
                                    'if-range': self.headers.get('If-Range'),
                                    'if-match': self.headers.get('If-Match')})
        self.send_response(status)
        self.send_header('Content-Type', 'video/mp4')
        self.send_header('Accept-Ranges', 'bytes')
        if self.server.kind != 'no-validator-cookie':
            self.send_header('ETag', '"compat-fixture-v1"')
        if self.server.kind in ('no-store-cookie', 'gzip'):
            self.send_header('Cache-Control', 'no-store')
        if encoded:
            self.send_header('Content-Encoding', 'gzip')
        if self.server.cookie and not start:
            self.send_header('Set-Cookie', 'floe_test=valid; Path=/')
        if status == 206:
            self.send_header('Content-Range', 'bytes {}-{}/{}'.format(start, end - 1, len(self.server.payload)))
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def run_case(library, fixture, scratch, kind, enabled):
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    server.payload = fixture.read_bytes()
    server.kind = kind
    server.cookie = kind in ('cookie', 'cookie-file', 'no-store-cookie', 'no-validator-cookie')
    server.records = []
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = {key: value for key, value in os.environ.items()
           if key.lower() not in ('http_proxy', 'https_proxy', 'all_proxy', 'no_proxy')}
    url = 'http://127.0.0.1:{}/movie.mp4'.format(server.server_port)
    root = scratch / '{}-{}'.format(kind, enabled)
    if kind == 'unavailable-disk':
        root.write_bytes(b'ordinary file')
    properties = {'curl-enabled': 'no' if kind == 'ffmpeg' else 'yes',
                  'network-timeout': '2', 'curl-max-retries': '0',
                  'curl-connect-timeout': '2', 'cache': 'yes',
                  'stream-buffer-size': '4096', 'floe-cache-enabled': 'yes' if enabled else 'no',
                  'floe-cache-dir': str(root), 'floe-cache-url': url,
                  'floe-cache-source': SOURCE, 'floe-cache-key': hashlib.sha256(kind.encode()).hexdigest(),
                  'floe-cache-max-bytes': str(16 * 1024 * 1024)}
    if server.cookie:
        properties.update({'cookies': 'yes', 'start': '4', 'curl-max-request-size': '262144'})
    if kind == 'cookie-file':
        cookies = scratch / 'cookies.txt'
        cookies.write_text('# Netscape HTTP Cookie File\n127.0.0.1\tFALSE\t/\tFALSE\t0\tfloe_test\tstale\n', encoding='utf8')
        properties['cookies-file'] = str(cookies)
    if kind in ('environment-proxy', 'explicit-proxy'):
        url = 'http://floe-cache-fixture.invalid/movie.mp4'
        properties['floe-cache-url'] = url
        proxy = 'http://127.0.0.1:{}'.format(server.server_port)
        if kind == 'environment-proxy':
            env['http_proxy'] = proxy
        else:
            properties['http-proxy'] = proxy
        env['no_proxy'] = ''
    if kind == 'conditional-header':
        properties['http-header-fields'] = 'If-Match: "compat-fixture-v1"'
    if kind == 'local-file':
        url = str(fixture)
        properties['floe-cache-url'] = url
    try:
        child = subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                '--library', str(library), '--child'],
                               input=json.dumps({'url': url, 'properties': properties}),
                               text=True, capture_output=True, env=env, timeout=20)
        try:
            native = json.loads(child.stdout)
        except ValueError:
            native = {'stdout': child.stdout, 'stderr': child.stderr}
        chunks = list(root.rglob('*.chunk')) if root.is_dir() else []
        passed = (child.returncode == 0 and native.get('loaded') and native.get('end_reason') == 0
                  and all(record['status'] < 400 for record in server.records))
        bypass = not enabled or kind in ('ffmpeg', 'no-store-cookie', 'no-validator-cookie',
                                         'gzip', 'conditional-header', 'local-file', 'unavailable-disk')
        if bypass:
            passed = passed and not chunks and native.get('cache_state') is None
        else:
            passed = passed and bool(chunks)
        if kind == 'conditional-header':
            passed = passed and all(record['if-match'] == '"compat-fixture-v1"' for record in server.records)
        if kind not in ('local-file',):
            passed = passed and bool(server.records)
        for chunk in chunks:
            parts = chunk.name.split('.')
            data = chunk.read_bytes()
            start, length = int(parts[2]), int(parts[3])
            passed = passed and len(data) == length and hashlib.sha256(data).hexdigest() == parts[4]
            passed = passed and data == server.payload[start:start + length]
        if kind == 'unavailable-disk':
            passed = passed and root.read_bytes() == b'ordinary file'
        return {'case': kind, 'cache_enabled': enabled, 'passed': bool(passed),
                'exit_code': child.returncode, 'native': native,
                'requests': server.records, 'chunk_count': len(chunks)}
    finally:
        server.shutdown()
        server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--fixture', type=Path)
    parser.add_argument('--ffmpeg')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--cache', choices=('yes', 'no', 'both'), default='both')
    parser.add_argument('--child', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    library = args.library.resolve()
    if args.child:
        return player(library)
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
    results = []
    with tempfile.TemporaryDirectory(prefix='floe-cache-http-', dir=args.output_dir) as directory:
        scratch = Path(directory)
        fixture = args.fixture.resolve() if args.fixture else scratch / 'fixture.mp4'
        if not args.fixture:
            if not args.ffmpeg:
                parser.error('provide --fixture or --ffmpeg')
            subprocess.run([args.ffmpeg, '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                            '-i', 'testsrc2=size=160x120:rate=15', '-t', '8', '-c:v', 'mpeg4',
                            '-q:v', '3', '-movflags', '+faststart', '-an', str(fixture)],
                           check=True, timeout=30)
        for enabled in ([True, False] if args.cache == 'both' else [args.cache == 'yes']):
            for kind in args.case or CASES:
                result = run_case(library, fixture, scratch, kind, enabled)
                results.append(result)
                print(json.dumps({key: result[key] for key in
                                  ('case', 'cache_enabled', 'passed', 'exit_code', 'chunk_count')}), flush=True)
                if not result['passed']:
                    print(json.dumps(result), flush=True)
                if args.report:
                    args.report.write_text(json.dumps(results, indent=2), encoding='utf8')
    return int(any(not result['passed'] for result in results))


if __name__ == '__main__':
    sys.exit(main())
