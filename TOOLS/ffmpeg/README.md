# Bounded MP4 read policy

`0001-mov-bounded-interleave.patch` is the canonical companion FFmpeg patch for
`demuxer-lavf-mp4-read-strategy`. Apply it when building FFmpeg, then build mpv
against that library. It keeps FFmpeg's legacy ordering by default; mpv opts in
only for seekable HTTP(S) MOV/MP4 with a finite duration. Unpatched/system FFmpeg
continues to work and reports the unavailable extension at verbose log level.

The Windows build recipe carries an identical copy at
`packages/ffmpeg-0001-mov-bounded-interleave.patch`. Its FFmpeg revision remains
`f1126f23380c805b4b8fd69351d189aeac8b892b` (`RELEASE` is `8.0.git`). The additional
7.1.5 source compatibility probe does not change the Windows dependency revision;
that version has different MOV structures and is not supported by this patch.

## Design references

- [VLC MP4 DemuxMoov/DemuxTrack](https://github.com/videolan/vlc/blob/master/modules/demux/mp4/mp4.c):
  mature precedent for preferring physical file positions within bounded DTS
  preload on inputs where seeking is expensive. This implementation is
  independent; it does not copy VLC or Hills code.
- [FFmpeg MOV packet selection at the Windows pinned revision](https://github.com/FFmpeg/FFmpeg/blob/f1126f23380c805b4b8fd69351d189aeac8b892b/libavformat/mov.c):
  retain its legacy heuristic and explicit `interleaved_read=0` behavior as
  fallbacks. Plain physical order alone can pre-read an entire long track.
- [mpv's stream buffer documentation](https://github.com/mpv-player/mpv/blob/master/DOCS/man/options.rst):
  identifies small bidirectional seeks in some MP4 layouts. Increasing the
  low-level buffer alone does not solve tracks separated by large byte gaps.

## Behavior and limits

Header and stream probing keep their original behavior. The FFmpeg policy
activates only when pending selected audio/video samples are
at least 1 MiB apart. Inputs whose pending selected A/V positions stay closer
than that retain legacy packet ordering; large chunks can qualify too.
For separated tracks it reads in physical order within a **fixed** DTS window
(5 seconds by default). Each window has an 8 MiB default logical
preload byte budget. mpv additionally limits that budget to a quarter of the configured
forward demuxer cache and disables the feature for very small caches.

The byte count accumulates across packets from different tracks, including subtitles.
Exhausting the budget shortens the window to the last selected A/V track's next
DTS. Lagging tracks catch up before a new window opens; the window does not slide
forward one packet at a time. Sparse selected subtitles due within a window are
drained before advancing it. No packets are dropped and no packet payload, DTS,
PTS, index, decoding, rendering, or HTTP routing is rewritten. A large individual
packet and mandatory catch-up data can exceed the preload byte budget;
mpv's existing cache limit still applies. The scheduler allocates no packet cache.

Fragmented/live inputs, non-seekable inputs, external track IO, stream groups,
IAMF and invalid timestamps keep legacy scheduling. Degenerate non-advancing timestamps disable
the optimization until a seek. Seeking and track selection changes clear the
window; FFmpeg's seek cursor reconstruction still uses its original scheduler.
MKV, HLS and ISO are outside
the automatic MP4 opt-in. Explicit low-level FFmpeg interleave options take
precedence over mpv's automatic policy. Selected stream indices of 64 or above
retain legacy scheduling rather than use a lossy selection-change hash.

## Verification

The standalone test applies the actual companion patch to a **copy** of FFmpeg
source and compiles its production scheduling header and mpv scope policy:

```sh
python3 test/lavf_mp4_read.py --ffmpeg-source /path/to/ffmpeg
# Or obtain only the two required upstream files from GitHub:
python3 test/lavf_mp4_read.py --download-ref f1126f23380c805b4b8fd69351d189aeac8b892b
```

Requirements: Git, CMake and a C11 compiler. `--cmake` accepts an absolute path.
`--recipe /path/to/libmpv-win32-video-cmake` also verifies the two patches match.
The test covers ordinary interleaving fallback, the separated-track reproducer,
a two-hour input, byte-budget catch-up, interspersed subtitles/short audio runs,
sparse subtitles, negative DTS, ignored
tracks, seek reset, invalid timestamps, arithmetic limits and scope exclusions.
The production mpv/FFmpeg adapter functions are also compiled against small
test doubles, including an old FFmpeg layout without stream-group fields. These
checks cover scope, option precedence and unsupported-extension fallback; they
are not a full libmpv build or a real decoder/renderer test.

It does not build or replace the running app's DLL. After compiling the new
libmpv, verify real MP4 playback/seek/track switching plus the existing MKV,
ISO and streaming regressions before shipping the artifact.
