# Architecture

What the code is and why it is that way. `CLAUDE.md` has the rules for
changing it; `original/` is the program being ported, and its own long
comments are still the best account of why the map code does what it
does.

## The program

```
  gnss task (core 0, prio 5)       app_main loop
  UART1 -> nmea.h -> fix  ------>  follow the fix (mapview_centre)
  PPS on GPIO51                    render one queued tile (mapview_step)
                                     mapset -> maptile -> mapcore
                                   compose the window, marker, status
                                   blit (gfx, landscape)
```

- **mapcore** (`components/mapcore/`): the original's portable C, byte
  for byte -- PMTiles, gzip, MVT, the rasteriser, the style, the grid.
- **maptile** (`main/maptile.c`): one archive opened, one tile drawn.
  The middle of the original's netsource.cpp and render_tile().
- **mapset** (`main/mapset.c`): all the archives, and which one covers a
  tile. The original's "local archives".
- **mapview** (`main/mapview.c`): the 2 x 2 grid of 1280 px subtiles
  around the position, its queue, and the window of it on screen.
- **gnss** (`main/gnss.c`, `main/nmea.h`): the M135.
- **ffread** (`main/ffread.c`): a file read through FatFs directly.
- **aimless** (`main/aimless.c`): boot, the loop, the drawing.

Everything but gnss.c, ffread.c and aimless.c is free of ESP-IDF and
tested by `make -C test`.

## Milestones

1. **Offline map and position.** This.
2. **Tiles over Wi-Fi**, through feckless-network-handler: the
   original's netsource and tile cache, so the map works where the card
   has nothing. The render moves to its own task on core 1 then, as the
   original's worker is, because a network fetch must not hold the
   screen; saved networks either migrate from the original's NVS format
   or start fresh -- an open question.
3. **The rest**: labels and place names, zoom levels, the compass,
   waypoints, the setup portal, Wi-Fi location, AssistNow Autonomous,
   the world map floor, the night palette, exFAT for planet-sized files.

## Numbering

The patches are a series from 0001. Each one's number and subject is a
heading below, and the next patch takes the next number.

### 0001 -- original/, verbatim

m5tab5_m135_gnss_protomaps_live_area_map at 0cbd7b0, every tracked file
but the sunset submodule.

### 0002 -- mapcore, unchanged

The six portable C modules and their headers as a component, byte for
byte. They compile on a host with -Wall -Wextra and no warnings.

### 0003 -- maptile, and the first host test

An archive opened and a tile drawn, with the original's capacities, and
`test/fixture.pmtiles` -- synthetic, 3 x 3 tiles at z14 -- to run the
whole offline path on under the sanitizers.

One thing mapcore does that C11 does not allow: raster.c shifts
negative fixed-point values left. Every compiler this targets does the
arithmetic thing, and mapcore stays unchanged, so the test turns that
one sanitizer check off and says so.

### 0004 -- gnss

The NMEA parser as a header, host-tested on sentences with real
checksums; the UART task on IDF's driver; PPS in a GPIO interrupt;
UBX-CFG-RATE. AssistNow and the navigation database are milestone 3.

### 0005 -- milestone 1

mapset, mapview, ffread and the app; the build files; this file.

**Archives.** Every `.pmtiles` in the root of the card, then of a USB
drive, up to 16. A tile is asked of the archives whose header covers it
-- zoom range and bounding box -- in that order, until one has it.

**Reading.** Through FatFs, not stdio. `fseek()` takes a `long`, 32 bits
on this target, so stdio stops at 2 GB; plan-extracts.py sizes bands to
fit FAT32's 4 GB, so an extract is often between the two. `f_lseek()`
reaches the whole 4 GB. Fast seek is on (sdkconfig.defaults), because
without it the original measured ~500 ms a seek on a large archive. Each
read holds feckless-storage's arbiter at the PLAYBACK class, a chunk at a
time. Files over 4 GB need exFAT, which needs the player's patched FatFs;
milestone 3.

**The grid.** The original's: GRID_N 2, SUBTILE_PX 1280, z14 with no
split, so 13 MB of PSRAM for the four tile buffers. When the position is
more than half a tile from the grid's centre, tile_grid.c shifts it,
keeping what it still covers; a jump of more than two tiles starts a new
grid. The queue is rebuilt after every move from whatever is PENDING,
nearest first: a job queued before a shift carries the old generation
and would be refused, leaving a surviving slot pending for ever.

**One task.** The loop renders one tile, then draws. A tile takes
several hundred milliseconds in the original's measurements, so the
screen updates in steps while a grid fills. The GNSS reader is its own
task and never waits for this. The original's separate render worker
comes back in milestone 2.

**The screen.** gfx at GFX_ROT_270, the way up lothesome found for the
original's setRotation(3). Each frame is a full-screen compose and a
rotated blit: fine at once a second plus once a tile, and the first
thing to measure if the screen ever needs to follow faster.

**Before a fix** the map shows the first archive's header centre, with
no marker: a position nobody measured is not drawn as one.

Not built against ESP-IDF here. aimless.c, gnss.c and ffread.c were
type-checked on the host against stub headers only. On the board, look
for the banner, a line per archive found, "first fix after N ms", and
"tile in N ms" for each tile.
