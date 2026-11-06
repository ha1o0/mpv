#!/usr/bin/env python3
"""Render both subtitle slots with libmpv and check independent appearance.

Usage: python3 test/secondary_subtitle_style.py --library /path/to/libmpv
Uses private players, a generated black video and temporary SRT/ASS files.
No network, installed application preferences or screenshot files are used.
"""
import argparse
import ctypes as C
from pathlib import Path
import shutil
import tempfile
from floe_cache_preview import Player, Node


def screenshot(player):
    args = (C.c_char_p * 3)(b'screenshot-raw', b'subtitles', None)
    node = Node()
    assert player.lib.mpv_command_ret(player.handle, args, C.byref(node)) >= 0
    try:
        fields = node.u.list.contents
        values = {fields.keys[i].decode(): fields.values[i] for i in range(fields.num)}
        assert values['format'].u.string == b'bgr0'
        stride = values['stride'].u.int64
        width = values['w'].u.int64
        height = values['h'].u.int64
        data = values['data'].u.bytes.contents
        pixels = C.string_at(data.data, data.size)
        # Ignore the unused fourth channel and row padding in BGR0 buffers.
        rgb = b''.join(pixels[y * stride + channel:y * stride + width * 4:4]
                       for y in range(height) for channel in range(3))
        return rgb[:len(rgb) // 2], rgb[len(rgb) // 2:]
    finally:
        player.lib.mpv_free_node_contents(C.byref(node))


def set_option(player, name, value):
    player.command('set', name, str(value))


def frame(player):
    player.frame('seek', '1', 'absolute+exact')
    return screenshot(player)


def test_style(library, directory, extension):
    primary = directory / ('primary.' + extension)
    secondary = directory / ('secondary.' + extension)
    for path, title in [(primary, 'PRIMARY ABC xyz'), (secondary, 'SECONDARY ABC xyz')]:
        if extension == 'srt':
            path.write_text('1\n00:00:00,000 --> 00:00:20,000\n' + title + '\n', encoding='utf8')
        else:
            path.write_text('''[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360
[V4+ Styles]
Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,Alignment,MarginL,MarginR,MarginV,Encoding
Style: Default,sans-serif,30,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,2,10,10,10,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.00,0:00:20.00,Default,,0,0,0,,''' + title + '\n', encoding='utf8')
    player = Player(library, **{'sub': 'auto', 'sub-ass-override': 'force',
        'secondary-sub-ass-override': 'force', 'sub-color': '#FFFFFF00',
        'sub-font-size': '36', 'sub-border-size': '0', 'sub-shadow-offset': '0',
        'sub-back-color': '#00000000', 'secondary-sub-pos': '10'})
    try:
        assert player.get('secondary-sub-style') == 'shared'
        assert player.get('secondary-sub-hdr-peak') == 'shared'
        assert player.get('secondary-image-subs-hdr-peak') == 'shared'
        player.frame('loadfile', 'av://lavfi:color=c=black:s=640x360:r=1:d=30')
        player.command('sub-add', str(primary), 'select')
        player.command('sub-add', str(secondary), 'auto')
        set_option(player, 'secondary-sid', '2')
        baseline = frame(player)
        assert any(baseline[0]) and any(baseline[1]), 'both subtitles must render'
        set_option(player, 'secondary-sub-color', '#FF00FF00')
        assert frame(player) == baseline, 'default shared styling changed'
        for key, value in {'secondary-sub-font-size': '36',
                           'secondary-sub-border-size': '0',
                           'secondary-sub-shadow-offset': '0',
                           'secondary-sub-back-color': '#FF0000FF',
                           'secondary-sub-style': 'independent'}.items():
            set_option(player, key, value)
        current = frame(player)
        assert current[0] != baseline[0] and current[1] == baseline[1]
        for key, value in [('secondary-sub-font-size', '48'),
                           ('secondary-sub-font', 'monospace'),
                           (('secondary-sub-bold', 'yes') if extension == 'srt' else
                            ('secondary-sub-ass-style-overrides', 'Bold=1')),
                           ('secondary-sub-border-size', '3'),
                           ('secondary-sub-border-color', '#FFFF0000'),
                           ('secondary-sub-shadow-offset', '5'),
                           ('secondary-sub-back-color', '#FFFF00FF')]:
            set_option(player, key, value)
            changed = frame(player)
            assert changed[0] != current[0], (extension, key, 'secondary did not change')
            assert changed[1] == baseline[1], (extension, key, 'primary changed')
            assert player.get('sid') == 1 and player.get('secondary-sid') == 2
            current = changed
        for key, value in [('sub-font-size', '28'), ('sub-color', '#FFFF00FF'),
                           (('sub-bold', 'yes') if extension == 'srt' else
                            ('sub-ass-style-overrides', 'Bold=1'))]:
            set_option(player, key, value)
            changed = frame(player)
            assert changed[0] == current[0], (key, 'secondary changed with primary')
            assert changed[1] != current[1], (key, 'primary did not change')
            current = changed
        for key, value in [('secondary-sub-hdr-peak', '300'),
                           ('secondary-image-subs-hdr-peak', 'video-dynamic')]:
            set_option(player, key, value)
            expected = 300 if value == '300' else value
            assert player.get(key) == expected
        assert player.get('sub-hdr-peak') == 'sdr'
        assert player.get('image-subs-hdr-peak') == 1000
        print('PASS independent {} styles, shared defaults, both slots and HDR options'.format(extension))
    finally:
        player.close()


def test_bitmap_scale(library, directory):
    samples = Path(__file__).resolve().parent / 'samples'
    for slot in ('primary', 'secondary'):
        for extension in ('idx', 'sub'):
            shutil.copyfile(samples / ('img_sub.' + extension),
                            directory / ('bitmap-' + slot + '.' + extension))
    player = Player(library, **{'sub': 'auto', 'sub-ass-override': 'force',
                    'secondary-sub-ass-override': 'force', 'secondary-sub-pos': '10'})
    try:
        player.frame('loadfile', 'av://lavfi:color=c=black:s=640x360:r=1:d=30')
        player.command('sub-add', str(directory / 'bitmap-primary.idx'), 'select')
        player.command('sub-add', str(directory / 'bitmap-secondary.idx'), 'auto')
        set_option(player, 'secondary-sid', '2')
        baseline = frame(player)
        assert any(baseline[0]) and any(baseline[1]), 'both bitmap subtitles must render'
        set_option(player, 'secondary-sub-scale', '2')
        secondary_scaled = frame(player)
        assert secondary_scaled[0] != baseline[0], 'secondary bitmap scale did not change'
        assert secondary_scaled[1] == baseline[1], 'primary bitmap scale changed'
        set_option(player, 'sub-scale', '2')
        both_scaled = frame(player)
        assert both_scaled[0] == secondary_scaled[0], 'primary scale changed secondary'
        assert both_scaled[1] != baseline[1], 'primary bitmap scale did not change'
        set_option(player, 'secondary-sub-scale', '-1')
        set_option(player, 'sub-scale', '1')
        assert frame(player) == baseline, 'legacy shared bitmap scaling was not restored'
        assert player.get('sid') == 1 and player.get('secondary-sid') == 2
        print('PASS independent bitmap scaling, shared fallback and track selection')
    finally:
        player.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mpv-secondary-style-') as directory:
        for extension in ('srt', 'ass'):
            test_style(args.library.resolve(), Path(directory), extension)
        test_bitmap_scale(args.library.resolve(), Path(directory))


if __name__ == '__main__':
    main()
