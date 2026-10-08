#!/usr/bin/env python3
"""
fetch_worldtile.py -- get tile z0/0/0, the whole world in one tile, out of
a PMTiles archive, for the firmware to embed and draw at boot (0020).

    fetch_worldtile.py OUT --base URL [--pinned NAME]
    fetch_worldtile.py OUT --file ARCHIVE.pmtiles [--zxy Z/X/Y]

With --base, the archive is the newest Protomaps daily build under URL
(or NAME.pmtiles with --pinned), read with HTTP range requests exactly as
netremote.c does on the device: probe back from today for a build that
answers, read its header, walk its directories to the tile, fetch the
tile. With --file, the same walk over a local archive -- what the host
test uses, on test/fixture.pmtiles.

OUT gets the tile's bytes as stored (gzip-compressed MVT). On any failure
it gets nothing at all -- an empty file -- and this exits 0: the
firmware then simply has no backdrop, which is what a build with no
network should produce, not a failed build.

Why at build time and not committed: the tile is OpenStreetMap-derived
data under the ODbL, and CLAUDE.md keeps that out of the repository. It
lands in the build directory and the firmware image.

Standard library only, so the IDF build's own Python runs it.

SPDX-License-Identifier: MIT
"""
import argparse
import datetime
import gzip
import os
import struct
import sys
import urllib.request

# src: the PMTiles v3 specification -- a 127-byte header.
HEADER_LEN = 127
# src: netremote.c MAX_PROBE_DAYS, the original's: Protomaps keep about a
# week of daily builds.
MAX_PROBE_DAYS = 8
# src: netremote.c HTTP_TIMEOUT_MS.
TIMEOUT_S = 15


class Source:
    """Byte ranges from a URL or a file."""

    def __init__(self, url=None, path=None):
        self.url, self.path = url, path

    def read(self, off, n):
        if self.path:
            with open(self.path, "rb") as f:
                f.seek(off)
                data = f.read(n)
        else:
            req = urllib.request.Request(
                self.url, headers={"Range": "bytes=%d-%d" % (off, off + n - 1),
                                   "User-Agent": "aimless-moving-map build"})
            with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
                if r.status != 206:
                    raise IOError("%s: HTTP %d, not 206" % (self.url, r.status))
                data = r.read()
        if len(data) != n:
            raise IOError("asked %d bytes at %d, got %d" % (n, off, len(data)))
        return data


def header(src):
    h = src.read(0, HEADER_LEN)
    if h[:7] != b"PMTiles" or h[7] != 3:
        raise ValueError("not a PMTiles v3 archive")
    (root_off, root_len, _meta_off, _meta_len, leaf_off, _leaf_len,
     data_off, _data_len) = struct.unpack_from("<8Q", h, 8)
    internal, tile_comp, tile_type = h[97], h[98], h[99]
    return dict(root_off=root_off, root_len=root_len, leaf_off=leaf_off,
                data_off=data_off, internal=internal, tile_comp=tile_comp,
                tile_type=tile_type)


def zxy_to_tileid(z, x, y):
    """src: mapcore's pmt_zxy_to_tileid(), the specification's Hilbert
    order after every lower zoom's tiles."""
    acc = sum(4 ** t for t in range(z))
    n, d, tx, ty = 1 << z, 0, x, y
    s = n // 2
    while s > 0:
        rx = 1 if tx & s else 0
        ry = 1 if ty & s else 0
        d += s * s * ((3 * rx) ^ ry)
        if ry == 0:
            if rx == 1:
                tx, ty = s - 1 - tx, s - 1 - ty
            tx, ty = ty, tx
        s //= 2
    return acc + d


def varints(buf, pos, count):
    out = []
    for _ in range(count):
        v, shift = 0, 0
        while True:
            b = buf[pos]
            pos += 1
            v |= (b & 0x7F) << shift
            if not b & 0x80:
                break
            shift += 7
        out.append(v)
    return out, pos


def directory(raw, internal):
    # src: the specification's compression codes: 1 none, 2 gzip.
    if internal == 2:
        raw = gzip.decompress(raw)
    elif internal != 1:
        raise ValueError("directory compression %d not handled" % internal)
    (n,), pos = varints(raw, 0, 1)
    deltas, pos = varints(raw, pos, n)
    runs, pos = varints(raw, pos, n)
    lens, pos = varints(raw, pos, n)
    offs, pos = varints(raw, pos, n)
    entries, tid = [], 0
    for i in range(n):
        tid += deltas[i]
        off = offs[i] - 1 if offs[i] else entries[-1][2] + entries[-1][3]
        entries.append((tid, runs[i], off, lens[i]))
    return entries


def find(src, h, tid):
    """The tile's (offset, length) in the archive, or None."""
    off, n = h["root_off"], h["root_len"]
    for _depth in range(4):     # src: mapcore pmt_find(): at most 4 levels
        best = None
        for e in directory(src.read(off, n), h["internal"]):
            if e[0] <= tid:
                best = e
            else:
                break
        if best is None:
            return None
        e_tid, run, e_off, e_len = best
        if run == 0:            # a leaf directory
            off, n = h["leaf_off"] + e_off, e_len
            continue
        if tid - e_tid < run:
            return h["data_off"] + e_off, e_len
        return None
    return None


def tile(src, z, x, y):
    h = header(src)
    # src: the specification's tile type 1, MVT; compression 2, gzip --
    # what maptile.c's maprender_payload() takes.
    if h["tile_type"] != 1 or h["tile_comp"] != 2:
        raise ValueError("tiles are not gzip MVT (type %d, compression %d)"
                         % (h["tile_type"], h["tile_comp"]))
    hit = find(src, h, zxy_to_tileid(z, x, y))
    if not hit:
        raise ValueError("no tile %d/%d/%d in the archive" % (z, x, y))
    return src.read(*hit)


def newest(base, pinned):
    if pinned:
        return [base + pinned + ".pmtiles"]
    today = datetime.datetime.now(datetime.timezone.utc).date()
    return [base + (today - datetime.timedelta(days=b)).strftime("%Y%m%d") + ".pmtiles"
            for b in range(MAX_PROBE_DAYS)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--base")
    ap.add_argument("--pinned", default="")
    ap.add_argument("--file")
    ap.add_argument("--zxy", default="0/0/0")
    a = ap.parse_args()
    z, x, y = (int(v) for v in a.zxy.split("/"))

    data, said = b"", ""
    try:
        if a.file:
            data = tile(Source(path=a.file), z, x, y)
            said = a.file
        else:
            for url in newest(a.base, a.pinned):
                try:
                    data = tile(Source(url=url), z, x, y)
                    said = url
                    break
                except Exception as e:      # this build, not the next
                    print("worldtile: %s: %s" % (url, e), file=sys.stderr)
    except Exception as e:
        print("worldtile: %s" % e, file=sys.stderr)
        data = b""

    tmp = a.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, a.out)
    if data:
        print("worldtile: %d/%d/%d, %d bytes, from %s" % (z, x, y, len(data), said))
    else:
        print("worldtile: no tile; the firmware will have no world backdrop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
