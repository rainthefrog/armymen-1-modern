#!/usr/bin/env python3
"""Build the Army Men II sound pack for the mod's [audio] am2_sounds option (2026-11-21).

Reads the Army Men 1 effects archive (Audio\\sounds.asd) and the Army Men II effect wavs (audio\\*.wav), and
writes <out>/<id>_<am2 name>.wav (22050 Hz, 16-bit, mono PCM) plus <out>/map.txt, which audio.c reads.

    docker run --rm -v "<REA>\\Army Men:/am1:ro" -v "<REA>\\Army Men II:/am2:ro" -v "<REA>\\am2_sounds:/out" \\
        -v "<mod>\\scripts:/s:ro" armymen-re:2 python3 -I /s/am2_sounds.py /am1/Audio/sounds.asd /am2/audio /out

sounds.asd (AM1, "V0.0" archive built by the 3DO BldSnd tool from sounds.opt):
    0x000  char[4] "V0.0"; u32 count (65)
    0x008  count x { u32 sound id, u32 file offset }, the rest of the 0x800-byte table is 0xFF
    entry: "fmt " u32 16, PCMWAVEFORMAT (16 bytes), "data" u32 n, n bytes of PCM (no RIFF header)
    ids 1000..1064 = game table (BAZOOKA_EXPLODE .. TROOPER_DIE8); formats vary per sound (mostly 11025 Hz 8-bit
    mono; some 22050 / 44100 Hz 16-bit, two stereo). The game creates one DirectSound buffer per sound with
    that sound's own format, so any PCM format plays; the pack uses 22050 Hz 16-bit mono (AM II's rate, a
    format AM1 itself uses, e.g. 1012 EXPLODE_TROOPER).

Loudness: each output is scaled so its "active" RMS (20 ms windows within 20 dB of the loudest one) equals
that of the AM1 sound it replaces, limited so the peak stays below -0.3 dBFS. Pure standard library."""
import array, math, os, struct, sys

RATE = 22050

# AM1 sound (game table id, name) -> AM II wav, chosen by role. None = keep the AM1 sound.
# Voices / radio speech (voices.asd) and music (streams.asd) are never touched.
MAP = [
    (1000, "BAZOOKA_EXPLODE",     "explosiongrenade.wav", "bazooka shell explodes (AM II has no own bazooka blast)"),
    (1001, "MORTAR_EXPLODE",      "explosionmortar.wav",  "mortar shell explodes"),
    (1002, "GRENADE_EXPLODE",     "explosiongrenade.wav", "grenade explodes"),
    (1003, "MINE_EXPLODE",        "explosionbarrel.wav",  "mine explodes"),
    (1004, "BIG_EXPLOSION",       "explosionm80.wav",     "air strike bombs, large blasts, destroyed vehicles"),
    (1005, "BARREL_EXPLODE",      "explosionbarrel.wav",  "fuel barrel explodes"),
    (1006, "ROCK_HIT",            None, "bullet ricochet off rock"),
    (1007, "WATER_EXPLODE",       "explosionwater.wav",   "explosion in water"),
    (1008, "BULLET_HIT_OBJECT",   None, "bullet hits an object (also menu button sounds)"),
    (1009, "WATER_HIT",           None, "object falls into water"),
    (1010, "DESTROYED",           None, "building / bridge / fence destroyed"),
    (1011, "BULLET_HIT_TROOPER",  "trooperhit.wav",       "trooper hit by a bullet"),
    (1012, "EXPLODE_TROOPER",     None, "trooper blown up"),
    (1013, "FLAME_HIT_TROOPER",   None, "trooper / Sarge burning (AM II burndie.wav is the same recording)"),
    (1014, "BULLET_HIT_SARGE",    "sargehit.wav",         "Sarge hit by a bullet"),
    (1015, "SQUASH_SARGE",        None, "Sarge run over"),
    (1016, "SQUASH_TROOPER",      None, "trooper run over"),
    (1017, "HIT_TROOPER1",        None, "(unused by the game code)"),
    (1018, "HIT_TROOPER2",        None, "(unused by the game code)"),
    (1019, "VEHICLE_HIGH_IMPACT", "tankcollide.wav",      "vehicle hard collision"),
    (1020, "VEHICLE_LOW_IMPACT",  "jeepcollide.wav",      "vehicle light collision"),
    (1021, "BULLET_HIT_VEHICLE",  None, "bullet hits a vehicle"),
    (1022, "TRUCK_FULL_LOOP",     None, "truck engine loop (AM II has no truck)"),
    (1023, "HT_FULL_LOOP",        "htrackmove.wav",       "half-track engine loop (Sarge driving, volume = speed)"),
    (1024, "JEEP_FULL_LOOP",      "jeepmove.wav",         "jeep engine loop (Sarge driving, volume = speed)"),
    (1025, "TANK_FULL_LOOP",      "tankmove.wav",         "tank engine loop (Sarge driving; also menu screens)"),
    (1026, "FLAME_BEGIN",         "sargeflamefire.wav",   "flamethrower fires"),
    (1027, "FLAME_LOOP",          None, "flame loop"),
    (1028, "FLAME_END",           None, "flame end"),
    (1029, "HEAVY_MG",            "sargeheavymg.wav",     "heavy machine gun"),
    (1030, "LIGHT_MG",            None, "light machine gun (jeep gun; AM II vehiclefire.wav is empty)"),
    (1031, "MEDIUM_MG",           None, "medium machine gun / guard tower gun"),
    (1032, "CANNON",              "tankfire.wav",         "tank cannon"),
    (1033, "BAZOOKA",             "sargebaz.wav",         "bazooka fires"),
    (1034, "MORTAR",              "sargemofire.wav",      "mortar fires"),
    (1035, "MORTAR_WHISTLE",      None, "mortar shell whistle"),
    (1036, "GRENADE",             "sargeprime.wav",       "grenade pin pulled (throw)"),
    (1037, "RIFLE",               "sargerifle.wav",       "rifle and auto rifle shot (one id for both)"),
    (1038, "TOWER_GUN",           None, "tower rifle"),
    (1039, "PLANE_SOUND",         "planeoverhead.wav",    "air support plane overhead"),
    (1040, "WEAPON_ACTIVATE",     None, "weapon change / load"),
    (1041, "STEPS",               None, "footsteps (AM II sargewalk.wav is empty)"),
    (1042, "SWAMP_STEPS",         None, "footsteps in swamp"),
    (1043, "DESERT_SOUND1",       None, "desert wind"),
    (1044, "DESERT_SOUND2",       None, "vulture"),
    (1045, "BAYOU_SOUND1",        None, "crickets"),
    (1046, "BAYOU_SOUND2",        None, "frog"),
    (1047, "ALPINE_SOUND1",       None, "birds"),
    (1048, "ALPINE_SOUND2",       None, "flowing water"),
    (1049, "BUTTONSPRINGIN",      "click.wav",            "menu button click"),
    (1050, "CHOPPER_LOOP",        None, "script: helicopter loop"),
    (1051, "AIR_RAID_LOOP",       None, "script: air raid siren"),
    (1052, "CHUGGA_LOOP",         None, "script: train / engine loop"),
    (1053, "TICKING_LOOP",        None, "script: ticking"),
    (1054, "ALARM_LOOP",          None, "script: alarm"),
    (1055, "SARGE_FULLY_LOADED",  None, "Sarge voice: already loaded (speech, kept)"),
    (1056, "PORTAL",              None, "script: portal"),
] + [(1056 + i, "TROOPER_DIE%d" % i, None,
      "death cry %d of 8 (troopers and Sarge, random); AM II has one: trooperdie.wav (commented in map.txt)" % i)
     for i in range(1, 9)]


def read_asd(path):
    d = open(path, "rb").read()
    if d[:4] != b"V0.0":
        raise SystemExit("%s: not a V0.0 archive" % path)
    n = struct.unpack_from("<I", d, 4)[0]
    out = {}
    for i in range(n):
        sid, off = struct.unpack_from("<II", d, 8 + 8 * i)
        if d[off:off + 4] != b"fmt ":
            raise SystemExit("sound %d: no fmt chunk at %#x" % (sid, off))
        flen = struct.unpack_from("<I", d, off + 4)[0]
        tag, ch, rate, _, _, bits = struct.unpack_from("<HHIIHH", d, off + 8)
        p = off + 8 + flen
        n_data = struct.unpack_from("<I", d, p + 4)[0]
        out[sid] = (tag, ch, rate, bits, d[p + 8:p + 8 + n_data])
    return out


def read_wav(path):
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise ValueError("not a RIFF WAVE file")
    p, fmt, data = 12, None, None
    while p + 8 <= len(d):
        cid, size = d[p:p + 4], struct.unpack_from("<I", d, p + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", d, p + 8)
        elif cid == b"data":
            data = d[p + 8:p + 8 + size]
        p += 8 + size + (size & 1)
    if not fmt or data is None:
        raise ValueError("no fmt / data chunk")
    tag, ch, rate, _, _, bits = fmt
    return (tag, ch, rate, bits, data)


def to_float(tag, ch, rate, bits, raw):
    """PCM -> mono floats (-1..1) at RATE."""
    if tag != 1 or bits not in (8, 16):
        raise ValueError("unsupported format tag %d / %d bits" % (tag, bits))
    if bits == 8:
        s = [(b - 128) / 128.0 for b in raw]
    else:
        a = array.array("h", raw[:len(raw) // 2 * 2])
        if sys.byteorder != "little":
            a.byteswap()
        s = [v / 32768.0 for v in a]
    if ch == 2:
        s = [(s[i] + s[i + 1]) * 0.5 for i in range(0, len(s) - 1, 2)]
    if rate != RATE and len(s) > 1:                      # linear resampling (only odd files need it)
        n = int(len(s) * RATE / rate)
        step = (len(s) - 1) / max(n - 1, 1)
        r = []
        for i in range(n):
            x = i * step
            j = int(x)
            f = x - j
            r.append(s[j] * (1 - f) + s[min(j + 1, len(s) - 1)] * f)
        s = r
    return s


def active_rms(s, rate=RATE):
    w = max(1, rate // 50)                               # 20 ms windows
    rms = []
    for i in range(0, len(s), w):
        seg = s[i:i + w]
        rms.append(math.sqrt(sum(v * v for v in seg) / len(seg)))
    if not rms:
        return 0.0
    top = max(rms)
    act = [r for r in rms if r >= top * 0.1]
    return math.sqrt(sum(r * r for r in act) / len(act))


def write_wav(path, s):
    a = array.array("h", (max(-32768, min(32767, int(round(v * 32767)))) for v in s))
    if sys.byteorder != "little":
        a.byteswap()
    data = a.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: am2_sounds.py <AM1 sounds.asd> <AM II audio dir> <out dir>")
    asd_path, am2_dir, out = sys.argv[1:]
    am1 = read_asd(asd_path)
    os.makedirs(out, exist_ok=True)
    lines = ["# Army Men II sounds for Army Men 1 - read by the mod's audio.c when [audio] am2_sounds=1.",
             "# Built by scripts/am2_sounds.py (re-run it to rebuild; edit this file to change the choice).",
             "# <AM1 sound id> <file in this folder>   # AM1 name - role. A missing file = the original sound.",
             "# Formats: any RIFF PCM wav (8/16-bit, mono/stereo, any rate) - the pack uses 22050 Hz 16-bit mono.", ""]
    made = 0
    for sid, name, src, role in MAP:
        if sid not in am1:
            raise SystemExit("sound %d (%s) not in %s" % (sid, name, asd_path))
        a1 = am1[sid]
        if src is None:
            lines.append("# %d keep          # %s - %s" % (sid, name, role))
            continue
        s = to_float(*read_wav(os.path.join(am2_dir, src)))
        if not s:
            raise SystemExit("%s is empty" % src)
        target = active_rms(to_float(*a1))
        have = active_rms(s)
        peak = max(abs(v) for v in s) or 1.0
        gain = target / have if have > 0 else 1.0
        limited = gain * peak > 0.966
        if limited:
            gain = 0.966 / peak
        s = [v * gain for v in s]
        fname = "%d_%s" % (sid, src.lower())
        write_wav(os.path.join(out, fname), s)
        made += 1
        lines.append("%d %-26s # %s - %s" % (sid, fname, name, role))
        print("%d %-19s <- %-20s %5.2fs  gain %5.2f (%+.1f dB)%s  AM1 %d Hz %d-bit %.2fs" % (
            sid, name, src, len(s) / RATE, gain, 20 * math.log10(gain), " peak-limited" if limited else "",
            a1[2], a1[3], len(a1[4]) / (a1[1] * a1[2] * a1[3] / 8)))
    if os.path.exists(os.path.join(am2_dir, "trooperdie.wav")):     # optional, not mapped by default
        s = to_float(*read_wav(os.path.join(am2_dir, "trooperdie.wav")))
        target = active_rms(to_float(*am1[1057]))
        gain = min(target / active_rms(s), 0.966 / max(abs(v) for v in s))
        write_wav(os.path.join(out, "trooperdie.wav"), [v * gain for v in s])
        lines += ["", "# Death cries: AM1 picks one of 8 at random; AM II has a single one. Uncomment to use it",
                  "# for some or all of them (every death then sounds the same):"]
        lines += ["#%d trooperdie.wav" % sid for sid in range(1057, 1065)]
    with open(os.path.join(out, "map.txt"), "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")
    print("%d sounds written to %s (+ map.txt)" % (made, out))


if __name__ == "__main__":
    main()
