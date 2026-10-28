#!/usr/bin/env python3
"""Summarise a PS3 disc folder: PARAM.SFO fields, EBOOT container type, file sizes.

    disc_info.py [GAME_DIR]     (default: $KZ3_GAME or ./Game)
"""
import os, struct, sys


def read_sfo(path):
    d = open(path, "rb").read()
    _, _, kt, dt, n = struct.unpack("<4sIIII", d[:20])
    out = {}
    for i in range(n):
        ko, fmt, ln, _, do = struct.unpack("<HHIII", d[20 + 16 * i:36 + 16 * i])
        k = d[kt + ko:d.index(b"\0", kt + ko)].decode()
        v = d[dt + do:dt + do + ln]
        out[k] = struct.unpack("<I", v)[0] if fmt == 0x404 else v.rstrip(b"\0").decode(errors="replace")
    return out


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("KZ3_GAME", "Game")
    sfo = read_sfo(os.path.join(game, "PS3_GAME/PARAM.SFO"))
    for k in ("TITLE", "TITLE_ID", "VERSION", "APP_VER", "PS3_SYSTEM_VER", "CATEGORY", "RESOLUTION", "SOUND_FORMAT"):
        print(f"{k:16} {sfo.get(k)}")
    eboot = os.path.join(game, "PS3_GAME/USRDIR/EBOOT.BIN")
    head = open(eboot, "rb").read(4)
    kind = {b"SCE\0": "SELF (encrypted, needs decryption)", b"\x7fELF": "ELF (decrypted)"}.get(head, repr(head))
    print(f"{'EBOOT.BIN':16} {kind}, {os.path.getsize(eboot)} bytes")
    total = 0
    for dp, _, fs in os.walk(game):
        for f in fs:
            total += os.path.getsize(os.path.join(dp, f))
    print(f"{'disc total':16} {total / 2**30:.2f} GiB")


if __name__ == "__main__":
    main()
