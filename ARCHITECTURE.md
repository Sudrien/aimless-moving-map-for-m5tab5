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

### 0010 -- portalweb and dnsreply

Milestone 3 starts with the setup portal, so that a network can be added
here and not only in defeatist. This is its untrusted-input half,
host-tested before anything uses it.

**From defeatist, not from original/.** The original's portal.cpp is
Arduino WebServer and DNSServer, so it does not port. Defeatist's is
the same design rewritten on plain ESP-IDF for this board and this C6,
with everything that parses a byte from a phone pulled out to run under
the sanitizers: portalweb.c (the form's fields, HTML escaping, which
secret to try first) and dnsreply.c (the lie every captive portal tells
about every name). Both are copied from defeatist-music-player at
74535ca with their test, test/portalwebtest.c. The one change is the
access point's name, "Aimless-XXXX", so the two programs are told apart
in a phone's list; the test checks for it.

### 0011 -- launcherkey

The other way a network gets here: from M5Launcher, when this program
is installed under it. Launcher keeps its own list in an encrypted
`config.conf` on the card; defeatist has a one-tap import of it, and
this is that import's crypto core, split out so it runs on a host.

**The key is found, not stored.** Launcher's passwords are AES-128-CBC
under a key fixed at Launcher's build, which is in its firmware and not
its source. Every printable run in Launcher's app partition is tried,
and the one that decrypts config.conf's entries to valid padding over
printable text is it. launcherkey.h has the details.

**Three departures from defeatist's launcher_import.c**, each with a
check in test/launcherkeytest.c: the scan keeps its run across reads,
where defeatist's cut it at every 4 KB and so missed a key that
straddled one; the base64 decoder no longer overflows an int; and a PSK
saved in Launcher as 64 hex digits, 80 bytes encrypted, now fits.

The test's ciphertexts come from OpenSSL under a made-up key, with the
command in the test, so the decryption is checked against something
other than itself.

### 0012 -- the setup portal and the Launcher import

Milestone 3's first feature, on 0010 and 0011: a network can be added
here, from a phone, or copied from M5Launcher.

**When.** The original's rule: the portal opens when nothing is saved,
or when the screen is touched in the two seconds after it comes up
(original/tab5_map.cpp wantsSetup()). One step comes before it: with
nothing saved and this program started from M5Launcher, Launcher's
networks are imported first (launcher_import.c), and the portal opens
only if none came of it. The original also opened setup from its
settings panel and its download button; this has neither yet.

**It does not block.** The original's portal_run() owned the screen
for five minutes. Here the map keeps drawing and following the fix, the
render task keeps fetching, and a box along the bottom says what to do,
counts down, and closes setup on a tap. The box is drawn over each
composed frame from portal_state(), a copy.

**The radio.** wifi_enabled is true while setup wants the radio as well
as when something is saved, so with nothing saved the radio comes up
for the portal and goes back off when it closes. The network's
portal_running hook is true from the moment setup asks for the radio,
not only once the AP is up, so the background join stays out of the
portal's scan; the two-second window runs before boot's first
wifi_request_apply() for the same reason. GNSS now starts before the
window rather than after the radio request, so the receiver does not
lose the two seconds.

**From defeatist.** portal.c is its portal.c in setup mode only:
station mode and the stations form, translation and the playback pause
are left out, and the handlers' buffers are statics rather than
6144-byte-stack locals (one HTTP task, one handler at a time).
wifijoin.c is unchanged. launcher_import.c is its job around
launcherkey.c, with every buffer on the heap and config.conf read
through the arbiter. cJSON is IDF's `json` on 5.x and espressif/cjson
on 6.x, as defeatist's 6031: **the manifest changed**, so the next
build's dependencies.lock diff is read before it is committed (on 5.x
the rule is false and nothing should be added).

Touch is feckless-drivers' touch.c, already in the build; initialised
after the panel and turned with the picture (GFX_ROT_270). Without
touch the map runs and only the boot-time request is lost.

Not built against ESP-IDF here: the component registry cannot be
reached. Every new and changed file was compiled -fsyntax-only with
GCC 14.2 for the P4 (riscv32-esp-elf, -Os -Wall -Wextra -Werror
-Wformat-truncation) against IDF 5.5's own headers, the libraries at
their pinned tags, and an sdkconfig.h generated by kconfgen from
sdkconfig.defaults. The one error, xTaskCreatePinnedToCore() implicit
in aimless.c, is that setup's and is the same on the file before this
patch.

On the board: "setup: nothing saved; starting the portal" or "setup:
asked for by touch", the network's scan, "portal up: join Aimless-XXXX
and open http://192.168.4.1/ (N networks listed)", "submitted NAME:
password N bytes, ...", "trying NAME (...)", "saved NAME as PSK" or
"passphrase", "portal down (4)", "setup: Wi-Fi: saved NAME". Under
Launcher with nothing saved: "setup: nothing saved; asking M5Launcher",
"key recovered from partition '...'", a line per network, and "setup:
Imported N from M5Launcher, skipped M". What has not been seen on
hardware: everything above.

### 0013 -- out/, removed

0011 as first sent carried a copy of the 0010 patch file under `out/`,
left in the work tree when 0011 was committed. That copy went in with
it. This removes it; nothing else changes.

### 0014 -- the overview

Milestone 3's zoom work starts below the grid rather than above it.
The original reads the map at z14 only (its Z_LEVEL_CLOSE and
Z_LEVEL_WIDE are both Z_FLOOR), and the other zoom it draws is its
coarse overview: one z12 tile, 512 px, covering the 4 x 4 grid tiles
around the grid's middle, scaled up into any slot that has nothing of
its own. Before this, a tile not yet drawn, or with no data, was the
background colour; now it is the overview, soft, until the real tile
lands, and for good where none will.

**At compose time, not in the slot.** The original copied the overview
into a slot's buffer. Here the render task draws straight into a
PENDING slot's buffer (0009), so a copy there would be overwritten
half-way; mapview_compose() samples the overview instead, for the parts
of the window over a slot that is not drawable. Nearest-neighbour, a
source row resampled once and repeated, as the original's coarse_fill()
measured. Two 512 KB buffers, so the one composed is never the one being
drawn; with no PSRAM for them the map runs as it did.

**First in the queue.** The original queued the first overview behind
the grid because its boot screen already had a picture. This one has
none, so the overview goes first: one tile, then a whole soft screen.

**Retries** are the grid's: an overview that failed is asked for again
on the 30 s redo, and one with no data when the network appears
(mapview_redo()). It is drawn with the grid's render scratch at
COARSE_PX (maprender_resize(); only the coverage row depends on the
size, which maptiletest checks is exact) and through the same source
chain, so it comes from the cache, the card or the network.

COARSE_STEP 2 and COARSE_PX 512 are the original's. An archive with
z14 only has no z12, so on such a card the overview comes from the
network or not at all.

Checked on the host: mapviewtest works out every window pixel from the
geometry and compares, and the take/commit rules; maptiletest that a
resized scratch draws byte for byte what a native one does. aimless.c
compiled -fsyntax-only as 0012's, not built. On the board: "overview
12/x/y in N ms from card|cache|network: drawn" before the first tile.

### 0015 -- day and night

The original's automatic palette and backlight (original/README.md "Day
and night"): the night palette while the sun is down where the receiver
is, and the backlight in three steps, 24 % at night, 80 % by day, 55 % for
half an hour either side of sunrise and sunset. The levels are the
original's 60 and 140 of 255 as percent, judgements there; day is what
this program already used. The palette itself is mapcore's style.c,
which has had both since 0002.

**The sun** is sun.c, the sunrise equation in C, where the original used
buelowp/sunset (an Arduino library). Host-tested against astral's
times for nine places and dates: within 2.3 minutes, the test holding it
to three. Its wrap at UTC midnight and its circular distance to a
crossing are the original's. One difference: the library gave the same
time for rise and set with no crossing and the original had to call it
polar day; the equation tells polar night apart, and that is night.

**When.** Once a second, from the fix's position and RMC time, or the
last position and the SNTP clock. A fix dropping to 'V' changes nothing:
the original's palette flickered at walking pace until it stopped
treating a lost fix as news about the sun. With no position or no time
yet, nothing changes from the day palette.

**The render task changes the palette**, because the style is global
and it is what draws with it: between tiles, it re-initialises the
style and calls mapview_restyle(), which queues every drawn tile again
nearest first, drops the overview, and changes the background. An
overview being drawn across a restyle is thrown away when it commits.
Tiles with no data have no pixels and are left. So a switch costs a
redraw of the grid -- the original's cost too, once a day each way.

Not kept: the theme and brightness buttons (there are no buttons yet),
and the idle dim, which needs the accelerometer the original read.

Host: suntest (52 checks), and mapviewtest's restyle checks (9).
Device: aimless.c compiled -fsyntax-only as before, not built. On the
board: "sun: rise HH:MMZ set HH:MMZ at lat,lon; now day|night" when it
changes, then "palette: night|day".

### 0016 -- the button row, settings, pan and screen off

The original's footer row and settings panel, its 3 x 3 pan, and its
screen off, minus what belongs to features not here yet: the compass's
two settings rows (and the compass itself, which is not coming), place
names, Wi-Fi location, saved points and the area cache.

**uirow.c is the geometry**, with no drawing in it, so the hit tests run
on the host (uirowtest): the row's five slots (54 px, 12 px apart, the
touch zone 26 px up into the map and to the screen's edge), the pan
squares over the map between the status bar and that zone, the wake
zone (the middle ninth, where no button is), and the settings panel
(three quarters of the width, rows of 62 px, close at its bottom left).
Every number is the original's.

**Five slots, three buttons.** Home, settings and screen off are where
the original put them; the saved points and area cache slots stay empty
rather than the row closing up, so nothing moves when they arrive.

**Pan** is the original's anchor: the view follows an anchor that is the
marker until a tap on the map's edge moves it a third of the map
(MARKER_BAND) that way. While panned, fixes move the marker and not the
view, and a marker off the screen is not drawn. "recentre" is lit, as
there, because a panned view nobody remembers panning is the failure it
exists for; pressing it centres at once rather than at the next fix.
mapview_centre_tiles() takes the anchor, since it is not a position
anyone measured. The original also held the marker in a band before the
view moved; this still centres on it exactly, and that stays so.

**Settings**: the palette (auto, day, night), the backlight (auto, low,
medium, high -- the three automatic levels, as the original's
brightnessWanted()), and the Wi-Fi network, which opens 0012's portal.
Overrides cycle back to auto and are not kept across a restart, as
there. A fixed backlight overrides the palette's dimming as well.

**Screen off** draws the wake target for 700 ms and turns the backlight
to 0, as the original's screenOff(), and drops a pan. Unlike it, the
render task keeps drawing tiles, so the grid is ready on waking at the
cost of the power the original saved by stopping.

The panel and buttons are drawn over every composed frame, square
rather than rounded (gfx has no rounded rectangle). The setup box moves
up above the row's touch zone.

Host: uirowtest (73 checks) and one more in mapviewtest. Device:
aimless.c compiled -fsyntax-only as before, not built. On the board:
"pan: ...", "palette: auto|day|night", "brightness: N", "screen:
off|on", "setup: asked for from settings".

### 0017 -- saved points

The original's waypoints: up to 32 points, one target, and the
straight-line distance and bearing to it -- not routing, for its
reason: the archive is drawing geometry cut at tile edges, with no road
network to route on.

**waypoints.c** is original/waypoints.cpp as C with no file or clock in
it, host-tested (waypointstest): the list, the target kept on its point
through a removal, haversine distance and initial bearing, "name: 1.4 km
NE" with "here" under 30 m, and the file. **The file is the original's
/waypoints.bin byte for byte** -- "WPT1", version 1, a count, 48-byte
records -- checked against the original's own structs compiled on the
host, so points saved under the original are read here and the other
way round. A short file keeps its whole records; a bad header is an
empty list.

**On the card** it is written whole to waypoints.tmp and renamed over
waypoints.bin, where the original wrote in place: a cut mid-write then
leaves the old list. A name not given is the UTC time ("14:13") from the
fix or SNTP, or "pin N", as there (the original's was local time with
no zone set, which is UTC).

**On screen**, as the original's: the row's second slot ("points (N)",
or "to NAME" lit while guiding); a panel with save here, the list with
distance and bearing, delete, stop guiding, and up/down past a page; a
teardrop per point haloed in the palette's opposite, the target orange
and ringed; and a GUIDE_R arrow from the marker toward the target,
nothing when within 12 px. The target's line leads the status bar,
which is longer than the screen. Not kept: the original's edge arrows
for points off the screen.

Host: waypointstest (33 checks), uirowtest's panel checks (20).
Device: aimless.c compiled -fsyntax-only as before; not built. On the
board: "saved points: N" at boot, "added", "removed", "guiding to",
"wrote N".

### 0018 -- exFAT

The last of milestone 3's storage item: exFAT cards and drives, and so
archives over 4 GB, up to a planet build of about 126 GB in one file.

**Defeatist's machinery, unchanged.** IDF ships FatFs with exFAT
compiled out and no Kconfig switch for it. cmake/exfat.cmake runs
tools/enable_exfat.sh at the first configure, before project.cmake, to
copy IDF's fatfs into components/fatfs with FF_FS_EXFAT, FF_LBA64 and
FF_USE_TRIM 0 set, the Kconfig-guard fix that exFAT's code paths need,
and the 6035 hook that names a damaged exFAT entry (feckless-storage's
storage.c logs it). cmake/idfcopy.cmake records which IDF the copy came
from and refuses a configure under another. All three files are
defeatist's at 74535ca; components/fatfs is generated and ignored.
TAB5_NO_EXFAT in the environment opts out; README says why someone
might.

**Nothing in the map code changed.** ffread.c has read through FatFs's
f_lseek() since 0005 precisely so that this would be enough: FSIZE_t
becomes 64 bits with exFAT, and the offsets were uint64_t throughout,
mapcore's PMTiles reader included. The tile cache stays on stdio and
under 2 GB, which it always was (0008).

Checked here: enable_exfat.sh run against IDF v5.5's fatfs; the patched
ff.c and ffread.c compiled -fsyntax-only for the P4 with -Wall -Werror,
and FSIZE_t asserted to be 8 bytes with FF_FS_EXFAT 1. Not built with
idf.py. On the board, the first configure prints "exfat: patching a
local fatfs component", later ones "components/fatfs present"; an
exFAT card mounts and its archives are listed as before, with sizes
past 4096 MB.

The first build after this patch needs a clean configure (rm -rf build)
so that components/fatfs exists before IDF scans for components.
