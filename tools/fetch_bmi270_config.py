#!/usr/bin/env python3
"""
fetch_bmi270_config.py -- the BMI270's 8 KB configuration image, for the
firmware to embed and upload to the Tab5's own IMU at boot (0032).

    fetch_bmi270_config.py OUT
    fetch_bmi270_config.py OUT --file bmi270.c [--sha256 HEX]

Without --file, bmi270.c is read from Bosch's BMI270_SensorAPI at the
commit pinned below. Either way the bmi270_config_file[] array is taken
out of it, checked against the pinned SHA-256 (or --sha256), and written
to OUT as 8192 raw bytes.

The BMI270 has no usable image in ROM: until one is uploaded it answers
its chip ID and produces no data. The image is Bosch's, BSD-3-Clause, so
it is fetched at build time into the build directory rather than kept
in this repository, as tools/fetch_worldtile.py does for map data; a
firmware that carries it carries Bosch's notice in README.md.

On any failure OUT gets nothing -- an empty file -- and this exits 0: a
firmware without the image has no accelerometer, which is what a build
without network should produce, not a failed build.

Standard library only, so the IDF build's own Python runs it.

SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import re
import sys
import urllib.request

# src: github.com/boschsensortec/BMI270_SensorAPI master on 2026-10-09.
COMMIT = "41129fcfe39c583ee5462d79195741945d51c1fe"
URL = ("https://raw.githubusercontent.com/boschsensortec/BMI270_SensorAPI/"
       + COMMIT + "/bmi270.c")
# src: bmi270_config_file[] at COMMIT, measured with this script.
SHA256 = "2d75e68e343a13ff99be98261dfbd99d9e8c6f267da9e3c5aeb883276ad178db"
# src: bmi270.c, dev->config_size = sizeof(bmi270_config_file).
SIZE = 8192
# src: netremote.c HTTP_TIMEOUT_MS, as fetch_worldtile.py.
TIMEOUT_S = 15


def extract(source):
    """bmi270_config_file[]'s bytes from bmi270.c's text, or None."""
    m = re.search(r"bmi270_config_file\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", source, re.S)
    if not m:
        return None
    return bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{1,2})\b", m.group(1)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--file")
    ap.add_argument("--sha256", default=SHA256)
    a = ap.parse_args()

    data = b""
    try:
        if a.file:
            with open(a.file, encoding="utf-8", errors="replace") as f:
                text = f.read()
        else:
            req = urllib.request.Request(URL, headers={"User-Agent": "aimless-moving-map build"})
            with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
                text = r.read().decode("utf-8", errors="replace")
        image = extract(text)
        if image is None:
            print("bmi270: no bmi270_config_file[] in the source", file=sys.stderr)
        elif len(image) != SIZE:
            print("bmi270: image is %d bytes, not %d" % (len(image), SIZE), file=sys.stderr)
        elif hashlib.sha256(image).hexdigest() != a.sha256.lower():
            print("bmi270: image does not match the pinned SHA-256", file=sys.stderr)
        else:
            data = image
    except Exception as e:  # network, file: an empty image, not a failed build
        print("bmi270: %s" % e, file=sys.stderr)

    with open(a.out, "wb") as f:
        f.write(data)
    print("bmi270: %s" % ("%d bytes" % len(data) if data else
                          "none; the accelerometer is off in this build"), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
