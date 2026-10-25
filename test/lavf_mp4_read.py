#!/usr/bin/env python3
"""Apply the production FFmpeg patch to a copy and run its actual C policy.

Needs Python, Git, CMake and a C11 compiler. No media, accounts or player writes.
Pass --ffmpeg-source for offline use, or --download-ref for an upstream GitHub
commit/tag. This does not build or replace the application's libmpv DLL.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request


def run(command, cwd=None):
    env = os.environ.copy()
    if cwd:
        # Scratch may live inside another checkout's ignored build directory.
        # Stop Git discovering that parent: otherwise `git apply` can silently
        # skip patch paths outside its current repository-relative prefix.
        env["GIT_CEILING_DIRECTORIES"] = str(Path(cwd).resolve().parent)
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
            env.pop(name, None)
    p = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=180, env=env)
    if p.returncode:
        raise RuntimeError(p.stdout + p.stderr)
    return p.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--ffmpeg-source", type=Path)
    source.add_argument("--download-ref")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--recipe", type=Path, help="Check the Windows recipe's patch is identical")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    patch = repo / "TOOLS/ffmpeg/0001-mov-bounded-interleave.patch"
    if args.recipe:
        recipe_patch = args.recipe / "packages/ffmpeg-0001-mov-bounded-interleave.patch"
        assert patch.read_bytes() == recipe_patch.read_bytes(), "FFmpeg patches differ"
    # A fresh child directory keeps repeated tests from reapplying a patch to
    # user files or reusing stale generated headers. No source checkout is edited.
    parent = args.build_dir.resolve() if args.build_dir else Path(tempfile.gettempdir()).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="mp4-policy-", dir=parent)).resolve()
    assert scratch.is_relative_to(parent)
    src = scratch / "source"
    for name in ("libavformat/mov.c", "libavformat/isom.h"):
        destination = src / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if args.ffmpeg_source:
            shutil.copy2(args.ffmpeg_source / name, destination)
        else:
            url = f"https://raw.githubusercontent.com/FFmpeg/FFmpeg/{args.download_ref}/{name}"
            destination.write_bytes(urllib.request.urlopen(url, timeout=30).read())
    run(["git", "apply", "--check", str(patch)], src)
    run(["git", "apply", "--whitespace=error-all", str(patch)], src)
    mov = (src / "libavformat/mov.c").read_text(encoding="utf-8")
    # Review integration boundaries in addition to compiling the exact policy
    # shipped in FFmpeg: fragmented/explicit physical order stay legacy, seek
    # cursor reconstruction must never run the new packet-delivery scheduler.
    assert "mov->frag_index.nb_items || mov->fragment.moof_offset || mov->next_root_atom" in mov
    assert "!mov->interleaved_read" in mov
    seek = mov[mov.index("static int mov_read_seek("):]
    assert "ff_mov_bounded_reset(&mc->bounded_interleave);" in seek
    assert "AVIndexEntry *entry = mov_find_next_sample_legacy(s, &st);" in seek
    assert "ff_mov_bounded_record(&mov->bounded_interleave, st->index, pkt->size);" in mov
    shutil.copy2(repo / "test/lavf_mp4_read.c", scratch)
    shutil.copy2(repo / "test/lavf_mp4_adapter.c", scratch)
    begin = mov.index("static AVIndexEntry *mov_find_next_sample_legacy(")
    adapter = mov.index("static int mov_bounded_get_sample(", begin)
    end = mov.index("static int should_retry(", begin)
    # FFmpeg's unchanged legacy function uses a signed loop counter against
    # unsigned nb_streams. Suppress only that existing warning in this harness;
    # keep all new production code under the normal warnings-as-errors check.
    legacy_prefix = '''#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4018)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
'''
    legacy_suffix = '''#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
'''
    (scratch / "mov_adapter.inc").write_text(legacy_prefix + mov[begin:adapter] +
        legacy_suffix + mov[adapter:end], encoding="utf-8")
    mpv = (repo / "demux/demux_lavf.c").read_text(encoding="utf-8")
    opening = mpv.index("static int demux_open_lavf(")
    assert mpv.index("    setup_mp4_bounded_interleave(demuxer);", opening) > \
        mpv.index("avformat_find_stream_info(avfc, NULL)", opening)
    begin = mpv.index("static void setup_mp4_bounded_interleave(")
    end = mpv.index("static int demux_open_lavf(", begin)
    (scratch / "mpv_adapter.inc").write_text(mpv[begin:end], encoding="utf-8")
    (scratch / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.21)
project(lavf_mp4_read_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_executable(lavf_mp4_read_test lavf_mp4_read.c)
add_executable(lavf_mp4_adapter_test lavf_mp4_adapter.c)
add_executable(lavf_mp4_old_ffmpeg_test lavf_mp4_adapter.c)
target_compile_definitions(lavf_mp4_old_ffmpeg_test PRIVATE TEST_OLD_FFMPEG=1)
target_include_directories(lavf_mp4_read_test PRIVATE "'''+str(repo / "demux").replace("\\", "/")+'''" "${CMAKE_CURRENT_SOURCE_DIR}/source/libavformat")
target_include_directories(lavf_mp4_adapter_test PRIVATE "'''+str(repo / "demux").replace("\\", "/")+'''" "${CMAKE_CURRENT_SOURCE_DIR}/source/libavformat" "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(lavf_mp4_old_ffmpeg_test PRIVATE "'''+str(repo / "demux").replace("\\", "/")+'''" "${CMAKE_CURRENT_SOURCE_DIR}/source/libavformat" "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(lavf_mp4_read_test PRIVATE /W4 /WX)
  target_compile_options(lavf_mp4_adapter_test PRIVATE /W4 /WX)
  target_compile_options(lavf_mp4_old_ffmpeg_test PRIVATE /W4 /WX)
else()
  target_compile_options(lavf_mp4_read_test PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
  target_link_options(lavf_mp4_read_test PRIVATE -fsanitize=address,undefined)
  target_compile_options(lavf_mp4_adapter_test PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
  target_link_options(lavf_mp4_adapter_test PRIVATE -fsanitize=address,undefined)
  target_compile_options(lavf_mp4_old_ffmpeg_test PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
  target_link_options(lavf_mp4_old_ffmpeg_test PRIVATE -fsanitize=address,undefined)
endif()
''', encoding="utf-8", newline="\n")
    run([args.cmake, "-S", str(scratch), "-B", str(scratch / "build")])
    run([args.cmake, "--build", str(scratch / "build"), "--config", "Release"])
    for name in ("lavf_mp4_read_test", "lavf_mp4_adapter_test", "lavf_mp4_old_ffmpeg_test"):
        paths = [scratch / ("build/Release/"+name+".exe"),
                 scratch / ("build/"+name+".exe"), scratch / ("build/"+name)]
        binary = next(p for p in paths if p.exists())
        print(run([str(binary)]).strip())
    print(f"Applied patch and compiled production policy; artifacts: {scratch}")


if __name__ == "__main__":
    main()
