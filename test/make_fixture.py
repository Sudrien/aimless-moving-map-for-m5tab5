#!/usr/bin/env python3
"""
make_fixture.py -- write test/fixture.pmtiles: a 3 x 3 block of z14 tiles
of made-up streets, water, parks and buildings, in the Protomaps basemap's
layer and `kind` names, so the host test can run the whole pipeline the
firmware runs -- archive, directory, gzip, MVT decode, rasterise -- on a
file with the same structure as a real extract.

The output is committed, so the test needs only a C compiler. This script
is kept so the fixture can be regenerated and diffed; it needs

    pip install pmtiles mapbox-vector-tile

The geometry is synthetic on purpose: an OpenStreetMap extract would carry
ODbL obligations into the repository, and the test cares about the
pipeline, not about any real street.

The block is centred on tile 14/4823/6160 -- around 46.0 N, -74.0 W -- an
arbitrary spot chosen only so the coordinates are not zero.

SPDX-License-Identifier: MIT
"""
import gzip
import io
import math
import os
import random

import mapbox_vector_tile
from pmtiles.tile import Compression, TileType, zxy_to_tileid
from pmtiles.writer import Writer

Z = 14
CX, CY = 4823, 6160
EXTENT = 4096

random.seed(14)


def ring(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)]


def tile_layers(tx, ty):
    """One tile's features. Streets run on a global grid, so they line up
    across tile edges; everything else is per tile."""
    E = EXTENT
    feats = {"earth": [], "water": [], "landuse": [], "roads": [],
             "buildings": [], "pois": []}

    feats["earth"].append({"geometry": {"type": "Polygon",
                                        "coordinates": [ring(0, 0, E, E)]},
                           "properties": {"kind": "earth"}})

    # A river running diagonally through the middle tile column.
    if tx == CX:
        pts = []
        for i in range(0, 9):
            y = i * E / 8
            x = E * 0.35 + 300 * math.sin((ty * 8 + i) / 3.0)
            pts.append((x, y))
        left = pts
        right = [(x + 380, y) for x, y in reversed(pts)]
        poly = left + right + [left[0]]
        feats["water"].append({"geometry": {"type": "Polygon",
                                            "coordinates": [poly]},
                               "properties": {"kind": "river"}})

    # A park in one corner of some tiles.
    if (tx + ty) % 2 == 0:
        feats["landuse"].append({"geometry": {"type": "Polygon",
                                              "coordinates": [ring(2600, 2600, 3800, 3700)]},
                                 "properties": {"kind": "park"}})
    else:
        feats["landuse"].append({"geometry": {"type": "Polygon",
                                              "coordinates": [ring(300, 2800, 1500, 3900)]},
                                 "properties": {"kind": "grass"}})

    # Streets: minor every 512 units, a major road every 2048.
    for k in range(-1, 10):
        v = k * 512
        kind = "major_road" if k % 4 == 0 else "minor_road"
        feats["roads"].append({"geometry": {"type": "LineString",
                                            "coordinates": [(v, -64), (v, E + 64)]},
                               "properties": {"kind": kind}})
        feats["roads"].append({"geometry": {"type": "LineString",
                                            "coordinates": [(-64, v), (E + 64, v)]},
                               "properties": {"kind": kind}})
    feats["roads"].append({"geometry": {"type": "LineString",
                                        "coordinates": [(0, E), (E, 0)]},
                           "properties": {"kind": "path"}})

    # Buildings in the blocks, skipping the river and the parks.
    for bx in range(0, 8):
        for by in range(0, 8):
            if random.random() < 0.45:
                continue
            x0 = bx * 512 + 60 + random.randint(0, 60)
            y0 = by * 512 + 60 + random.randint(0, 60)
            w = random.randint(140, 330)
            h = random.randint(140, 330)
            feats["buildings"].append({"geometry": {"type": "Polygon",
                                                    "coordinates": [ring(x0, y0, x0 + w, y0 + h)]},
                                       "properties": {"kind": "building"}})

    feats["pois"].append({"geometry": {"type": "Point", "coordinates": (1800, 1300)},
                          "properties": {"kind": "cafe", "name": "Cafe %d" % ((tx * 3 + ty) % 97)}})
    return feats


def encode(tx, ty):
    layers = tile_layers(tx, ty)
    data = [{"name": name, "features": feats}
            for name, feats in layers.items()]
    # y_coord_down: MVT's own convention, which mvt.c expects.
    raw = mapbox_vector_tile.encode(
        data, default_options={"extents": EXTENT, "y_coord_down": True,
                               "quantize_bounds": None})
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=0) as g:
        g.write(raw)
    return out.getvalue()


def lon_of(x):
    return x / 2 ** Z * 360.0 - 180.0


def lat_of(y):
    n = math.pi - 2 * math.pi * y / 2 ** Z
    return math.degrees(math.atan(math.sinh(n)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "fixture.pmtiles")
    tiles = []
    for ty in range(CY - 1, CY + 2):
        for tx in range(CX - 1, CX + 2):
            tiles.append((zxy_to_tileid(Z, tx, ty), encode(tx, ty)))
    tiles.sort()
    with open(path, "wb") as f:
        w = Writer(f)
        for tid, blob in tiles:
            w.write_tile(tid, blob)
        w.finalize(
            {
                "tile_type": TileType.MVT,
                "tile_compression": Compression.GZIP,
                "min_zoom": Z,
                "max_zoom": Z,
                "min_lon_e7": int(lon_of(CX - 1) * 1e7),
                "max_lon_e7": int(lon_of(CX + 2) * 1e7),
                "min_lat_e7": int(lat_of(CY + 2) * 1e7),
                "max_lat_e7": int(lat_of(CY - 1) * 1e7),
                "center_zoom": Z,
                "center_lon_e7": int(lon_of(CX + 0.5) * 1e7),
                "center_lat_e7": int(lat_of(CY + 0.5) * 1e7),
            },
            {"name": "aimless test fixture", "attribution": "synthetic"},
        )
    print("wrote %s, %d bytes, %d tiles" % (path, os.path.getsize(path), len(tiles)))


if __name__ == "__main__":
    main()
