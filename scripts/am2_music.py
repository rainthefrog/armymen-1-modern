#!/usr/bin/env python3
"""Build the Army Men II music pack for the mod's [audio] am2_music option (2026-11-21).

Reads the Army Men 1 music (Audio\\streams.asd, for the loudness target) and the Army Men II tracks
(audio\\music\\*.wav) and writes <out>/<track>.wav (same rate / channels as AM II, 16-bit PCM, loudness matched to
the AM1 music) plus <out>/map.txt, which music.c reads. Needs numpy (image armymen-pose:1):

    docker run --rm -v "<REA>\\Army Men:/am1:ro" -v "<REA>\\Army Men II:/am2:ro" -v "<REA>\\am2_music:/out" \\
        -v "<mod>\\scripts:/s:ro" armymen-pose:1 python3 -I /s/am2_music.py /am1/Audio/streams.asd /am2/audio/music /out

streams.asd = the same "V0.0" archive as sounds.asd (see am2_sounds.py), ids 2000..2009 (game table), all
11025 Hz 8-bit mono. The game streams each piece from disk in 0.2 s chunks with the piece's own format, so the AM II
files keep theirs (11025 / 22050 Hz 16-bit stereo); only the level changes.

map.txt lines: <AM1 stream id> <missions> <file>. missions = * (any) or a list like 1-3,8 (gpGame MissionID:
campaign 1..28 - Desert 1-9, Alpine 10-18, Bayou 19-28 -, Boot Camp 50). The first line that matches the stream
id and the current mission decides; no line = the original AM1 music. Each mission has a low and a high intensity
stream (Campaign.cpn MUSIC lo hi; mostly the low one plays, the scripts may switch), each campaign one for its map
screen (DESERT1 / ALPINE1 / BAYOU1)."""
import os, struct, sys
import numpy as np

# (stream id, AM1 name, missions, AM II track, note); order matters (first match wins)
MAP = [
    (2000, "TITLE_MUSIC",   "*",     "title.wav",     "title / main menus"),
    (2001, "ALPINE1_MUSIC", "50",    "bootcamp.wav",  "Boot Camp (low intensity)"),
    (2002, "ALPINE2_MUSIC", "50",    "bootcamp.wav",  "Boot Camp (high intensity)"),
    (2001, "ALPINE1_MUSIC", "10-11", "homeland.wav",  "Alpine scenario 1"),
    (2001, "ALPINE1_MUSIC", "12-13", "hobby.wav",     "Alpine scenario 2"),
    (2001, "ALPINE1_MUSIC", "14-18", "tankstore.wav", "Alpine scenarios 3-4"),
    (2001, "ALPINE1_MUSIC", "*",     "homeland.wav",  "Alpine campaign map screen"),
    (2002, "ALPINE2_MUSIC", "*",     "igd.wav",       "Alpine high intensity"),
    (2003, "DESERT1_MUSIC", "1-3",   "areax.wav",     "Desert scenario 1"),
    (2003, "DESERT1_MUSIC", "4-5",   "airborne.wav",  "Desert scenario 2"),
    (2003, "DESERT1_MUSIC", "6-9",   "frontyard.wav", "Desert scenarios 3-4"),
    (2003, "DESERT1_MUSIC", "*",     "areax.wav",     "Desert campaign map screen"),
    (2004, "DESERT2_MUSIC", "*",     "fortress.wav",  "Desert high intensity"),
    (2005, "BAYOU1_MUSIC",  "19-20", "kitchen.wav",   "Bayou scenario 1"),
    (2005, "BAYOU1_MUSIC",  "21-22", "playset.wav",   "Bayou scenario 2"),
    (2005, "BAYOU1_MUSIC",  "23-28", "mania.wav",     "Bayou scenarios 3-4"),
    (2005, "BAYOU1_MUSIC",  "*",     "kitchen.wav",   "Bayou campaign map screen"),
    (2006, "BAYOU2_MUSIC",  "*",     "igd.wav",       "Bayou high intensity (scenarios 1-3)"),
    (2007, "BAYOU3_MUSIC",  "*",     "fortress.wav",  "Bayou high intensity (scenario 4)"),
]
KEEP = [(2008, "WIN_MUSIC", "mission won jingle (AM II has no short one)"),
        (2009, "LOSE_MUSIC", "mission lost jingle (AM II has no short one)")]


def read_asd(path):
    d = open(path, "rb").read()
    if d[:4] != b"V0.0":
        raise SystemExit("%s: not a V0.0 archive" % path)
    out = {}
    for i in range(struct.unpack_from("<I", d, 4)[0]):
        sid, off = struct.unpack_from("<II", d, 8 + 8 * i)
        flen = struct.unpack_from("<I", d, off + 4)[0]
        fmt = struct.unpack_from("<HHIIHH", d, off + 8)
        p = off + 8 + flen
        n = struct.unpack_from("<I", d, p + 4)[0]
        out[sid] = (fmt, d[p + 8:p + 8 + n])
    return out


def read_wav(path):
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise SystemExit("%s: not a RIFF WAVE file" % path)
    p, fmt, data = 12, None, None
    while p + 8 <= len(d):
        cid, size = d[p:p + 4], struct.unpack_from("<I", d, p + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", d, p + 8)
        elif cid == b"data":
            data = d[p + 8:p + 8 + size]
        p += 8 + size + (size & 1)
    return fmt, data


def to_float(fmt, raw):
    tag, ch, rate, _, _, bits = fmt
    if tag != 1 or bits not in (8, 16):
        raise SystemExit("unsupported format %r" % (fmt,))
    if bits == 8:
        a = (np.frombuffer(raw, np.uint8).astype(np.float32) - 128) / 128
    else:
        a = np.frombuffer(raw[:len(raw) // 2 * 2], "<i2").astype(np.float32) / 32768
    a = a[:len(a) // ch * ch].reshape(-1, ch)
    return a, rate


def active_rms(a, rate):
    """RMS over 50 ms windows within 20 dB of the loudest (mono mix)."""
    m = a.mean(axis=1)
    w = max(1, rate // 20)
    n = len(m) // w
    if n == 0:
        return float(np.sqrt(np.mean(m * m)))
    r = np.sqrt(np.mean(m[:n * w].reshape(n, w) ** 2, axis=1))
    act = r[r >= r.max() * 0.1]
    return float(np.sqrt(np.mean(act ** 2)))


def write_wav(path, a, rate):
    ch = a.shape[1]
    data = np.clip(np.round(a * 32767), -32768, 32767).astype("<i2").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, ch, rate, rate * ch * 2, ch * 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: am2_music.py <AM1 streams.asd> <AM II audio\\music dir> <out dir>")
    asd, src, out = sys.argv[1:]
    am1 = read_asd(asd)
    levels = [active_rms(*to_float(*am1[i])) for i in range(2000, 2008)]
    target = float(np.sqrt(np.mean(np.square(levels))))
    print("AM1 music level (active RMS, 2000..2007): %s -> target %.3f" % (" ".join("%.3f" % v for v in levels), target))
    os.makedirs(out, exist_ok=True)
    for name in sorted({m[3] for m in MAP}):
        a, rate = to_float(*read_wav(os.path.join(src, name)))
        have, peak = active_rms(a, rate), float(np.abs(a).max()) or 1.0
        gain = min(target / have, 0.966 / peak)
        write_wav(os.path.join(out, name), a * gain, rate)
        print("%-14s %5d Hz %d ch %6.1f s  gain %5.2f (%+.1f dB)%s" % (name, rate, a.shape[1], len(a) / rate, gain,
              20 * np.log10(gain), " peak-limited" if gain < target / have else ""))
    lines = ["# Army Men II music for Army Men 1 - read by the mod's music.c when [audio] am2_music=1.",
             "# Built by scripts/am2_music.py (re-run to rebuild; edit this file to change the choice).",
             "# <AM1 stream id> <missions: * or e.g. 1-3,8> <file in this folder>   # first matching line wins;",
             "# no line = the original AM1 music. Missions: Desert 1-9, Alpine 10-18, Bayou 19-28, Boot Camp 50.", ""]
    for sid, nm, ms, f, note in MAP:
        lines.append("%d %-6s %-14s # %s - %s" % (sid, ms, f, nm, note))
    lines.append("")
    for sid, nm, note in KEEP:
        lines.append("# %d keep                 # %s - %s" % (sid, nm, note))
    with open(os.path.join(out, "map.txt"), "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")
    print("map.txt written")


if __name__ == "__main__":
    main()
