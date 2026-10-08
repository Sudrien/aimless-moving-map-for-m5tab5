# Architecture

What the code is and why it is that way. `CLAUDE.md` has the rules for
changing it; `original/` is the program being ported, and its own long
comments are still the best account of why the map code does what it
does.

## The program

```
  gnss task (core 0, prio 5)       app_main loop
  UART1 -> nmea.h -> fix  ------>  follow the fix (mapview_centre)
  PPS on GPIO51                    compose the window, marker, status
                                   blit (gfx, landscape)
                                        |  s_lock around the view
  render task (core 1, prio 4)          |
  netremote_update: online? build?      |
  take the nearest job  <---------------+
  tilesrc: cache -> card -> network
    maptile -> mapcore
  commit
```

- **mapcore** (`components/mapcore/`): the original's portable C, byte
  for byte -- PMTiles, gzip, MVT, the rasteriser, the style, the grid.
- **maptile** (`main/maptile.c`): one archive opened, one tile drawn.
  The middle of the original's netsource.cpp and render_tile().
- **mapset** (`main/mapset.c`): all the archives, and which one covers a
  tile. The original's "local archives".
- **mapview** (`main/mapview.c`): the 2 x 2 grid of 1280 px subtiles
  around the position, its queue, and the window of it on screen.
- **tilesrc** (`main/tilesrc.c`): which source a tile comes from -- the
  cache, the card, the network. The original's netsource_get().
- **tilecache** (`main/tilecache.c`): tiles fetched over the network,
  kept on the card. The original's tilecache.cpp.
- **netremote** (`main/netremote.c`): the remote archive over HTTP range
  requests, which build, and its cache. The network half of the
  original's netsource.cpp.
- **gnss** (`main/gnss.c`, `main/nmea.h`): the M135.
- **ffread** (`main/ffread.c`): a file read through FatFs directly.
- **aimless** (`main/aimless.c`): boot, the loop, the drawing.

Everything but gnss.c, ffread.c, netremote.c and aimless.c is free of ESP-IDF and
tested by `make -C test`.

## Milestones

1. **Offline map and position.** 0005.
2. **Tiles over the network**, through feckless-network-handler: the
   original's netsource and tile cache, so the map works where the card
   has nothing, and the render on its own task on core 1. Saved
   networks are defeatist's (0007, 0009). 0008 and 0009.
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

### 0006 -- scan_volume's root path buffer

The first IDF build: `-Wformat-truncation` is an error there, and GCC
sizes `"%d:/"` for any int. The buffer is 16 bytes.

### 0007 -- the shared partition table

0005 gave this program a table of its own, with nvs at 0x9000 x 0x6000.
The original and defeatist both use the Arduino core's app3M_fat9M_16MB,
nvs at 0x9000 x 0x5000, and that is where their saved networks live.
Milestone 2 reads them from there, so the table is theirs again, byte for
byte. Flashing it over 0005 needs a full `idf.py flash`, which writes the
table; the app is 3 MB now, not 4.

### 0008 -- tilesrc and tilecache

The first half of milestone 2, all host-tested: where a tile comes from,
and the cache the network's tiles go into. Nothing reaches the network
yet; the app's source chain has the card in it and nothing else, and
draws as 0005 did.

**The order** is the original's netsource_get(): the cache, then the
card, then the network. A negative marker in the cache -- the network
said it has no such tile -- stops the search before the network, so
ocean is asked about once. What the network returns is cached only once
it has drawn, so a bad payload is asked for again rather than kept.

**The cache's files** are the original's, byte for byte: `<build>.dat`,
records each with a 16-byte header, and `<build>.idx`, the sorted index.
A card that cached tiles under the original keeps them. The index is
written every write while the cache is small and every 256 later; a
power cut costs a rescan of the records, not the tiles. stdio's offsets
are 32-bit here, so the blob stops at 2 GB -- 80000 entries, the
original's index size, is well under that.

**maprender_tile()** is now maprender_fetch() and maprender_payload(),
so a payload from the cache or the network draws the same way.
`builddate.h` is the original's date arithmetic for naming a daily
build, here so the test reaches it before the code that uses it. And
mapview takes a draw callback instead of the archive set, plus a
take/commit pair for the render worker to come.

### 0009 -- the network

Milestone 2's second half: tiles over Wi-Fi or a USB Ethernet cable,
and the render on its own task.

**Saved networks are defeatist's.** feckless-network-handler's wifistore,
read from the "defeatist" NVS namespace, which 0007 put where defeatist
keeps it: a network joined in the player is joined here. There is no
way to add one in this program yet -- that is the setup portal,
milestone 3. With none saved the radio stays off; a cable works anyway.

**The remote archive** is an ordinary maparchive_t whose read callback
is an HTTP range request (netremote.c), so the PMTiles reader is the
card's. The original's rules for the connection, kept: one socket held
between requests; it survives only a request that came back exactly as
asked (206, the right length, every byte read), since unread bytes in a
reused socket are the next reply; a failure on a reused socket is tried
once more on a fresh one; 150 ms between requests; ranges in 32 KB
pieces. Not kept: the original's memo of the last blob, which was for
the world-floor walk (milestone 3).

**Which build**: `AIMLESS_PINNED_BUILD` if set, else the newest daily
found by probing back up to 8 days from today's date -- from the fix's
RMC date or from SNTP, whichever comes first. Recorded in `/t/build.txt`
on the card and probed for again after 30 days; a new build removes the
old cache. `AIMLESS_TILE_BASE` is where; both are in menuconfig under
"Aimless Moving Map".

**The render task** is pinned to core 1 (the original's worker was), so
a network fetch never holds up the screen. It takes a job under a lock,
draws without it, and commits under it again; tile_grid.c's generation
check refuses a commit the grid has moved past. When the network
appears, every tile that had no data is queued again; tiles that failed
are queued again every 30 s.

**Without maps on the card** the program no longer stops: it says so
and waits for a network and a fix.

sdkconfig.defaults gains the player's esp_hosted and memory lines (rm
sdkconfig). The manifest gains feckless_network_handler v0.1.0, which
brings esp_hosted and the rest: read the lock diff.

On the board: "N saved networks", the network library's join lines,
"probing build YYYYMMDD: ok", "remote build ... open", "network up: N
tiles to try again", "tile z/x/y in N ms from network", and every ten
seconds a "net ..." line with the route, the build and the counts. The
render task logs its unused stack every 16 tiles.

`builddate.h`'s bd_name() needed the same room for GCC's
format-truncation check as 0006: the IDF build is the first to compile
it.
