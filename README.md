# Aimless Moving Map *for M5Tab5*

An offline moving map for the M5Stack Tab5 with the M135 GNSS module:
vector tiles from Protomaps PMTiles archives on the card, drawn on the
device, with your position on them. You know where you are; where you
are going is your own business.

![Synthetic test tiles, drawn by the map pipeline](docs/view.png)

*Not a photo and not a real place: the test fixture's made-up streets,
river and parks, drawn on a PC by the same code the firmware runs, in
the 1280 x 720 window the screen shows. The status bar and the position
marker are drawn on top of this on the device.*

A port of
[m5tab5_m135_gnss_protomaps_live_area_map](https://github.com/Sudrien/m5tab5_m135_gnss_protomaps_live_area_map),
kept unchanged in `original/` for reference, to plain ESP-IDF with no
Arduino core and no M5Unified, on four libraries pulled out of
[Defeatist Music Player for M5Tab5](https://github.com/Sudrien/defeatist-music-player-for-m5tab5):

| Library | For |
|---|---|
| [feckless-drivers-for-m5tab5](https://github.com/Sudrien/feckless-drivers-for-m5tab5) | the I2C bus and IO expanders, the USB host |
| [feckless-graphics-handler-for-m5tab5](https://github.com/Sudrien/feckless-graphics-handler-for-m5tab5) | the panel, the framebuffer, text |
| [feckless-storage-handler-for-m5tab5](https://github.com/Sudrien/feckless-storage-handler-for-m5tab5) | the microSD card and USB drives |
| [feckless-network-handler-for-m5tab5](https://github.com/Sudrien/feckless-network-handler-for-m5tab5) | Wi-Fi, saved networks, USB Ethernet |

## Status: milestone 2, and the start of 3

What works:

- Every `.pmtiles` file in the root of the card or a USB drive is opened,
  and each tile is drawn from whichever archive covers it.
- Where the card has nothing, tiles come over Wi-Fi or a USB Ethernet
  cable from a Protomaps build, and are kept on the card for next time.
- The position from the M135, followed as it moves, at zoom 14.
- A marker, blue with a good 3D fix and grey with a rough one, and a
  status line with position, satellites, HDOP, speed and UTC.
- Before the first fix, the map shows the first archive's centre, with
  no marker.
- Wi-Fi setup from a phone, and M5Launcher's saved networks imported.
- An overview a few zooms out fills in, softly, wherever a tile has not
  been drawn yet or has no data, so the screen is not blank while tiles
  arrive. It needs zoom 12 in an archive or from the network.
- Day and night: the palette follows the sun where you are, and the
  backlight dims at night, with a step between for half an hour either
  side of sunrise and sunset. It needs a fix, or the network's clock and
  an earlier fix.

- The original's button row and settings panel, and panning by touch.

- Saved points, and a bearing and distance back to one.

- Cards and USB drives in FAT32 or exFAT, and archives of any size.
- The whole world, tile z0/0/0, behind the boot screen, drawn by the
  map's own renderer. The build fetches it from the same tile server the
  map uses (it needs network once, at the first configure); without it
  the boot screen is black.

Not yet: labels, the area cache. See `ARCHITECTURE.md`.

## On the screen

Along the bottom, four buttons, where the original had them:

- **centred / recentre** -- lit while you have panned the map away from
  where you are; press it to follow your position again.
- **points (N) / to NAME** -- saved points. "save here" drops one at
  your position; tap a point to be guided to it, and again (or "stop
  guiding") to stop; "del" removes one. Up to 32, kept on the card in
  the original's `waypoints.bin`, so points saved under it are here too.
  While guiding, the point's name, distance and direction lead the
  status line, and an arrow from your position points at it -- a
  straight line, not a route: the map data has no road network to
  route on.
- **settings** -- the palette (auto, day, night), the brightness (auto,
  low, medium, high) and the Wi-Fi network. Tap a row to change it, and
  anywhere outside the panel to close it. The overrides last until the
  next restart.
- **screen off** -- the backlight goes off; GNSS and tile downloads carry
  on. Touch the middle of the screen to wake it.

Tap the map's edges and corners to pan a third of a screen that way. The
middle is left alone: that is where your position is drawn.

The empty place in the row is for the area cache, still to come.

## Networks

Saved Wi-Fi networks are shared with
[Defeatist Music Player](https://github.com/Sudrien/defeatist-music-player-for-m5tab5):
join a network there and this program joins it too, and the other way
round. A USB Ethernet adapter (ASIX, or Realtek in CDC-ECM mode) works
without anything saved.

To add a network here, choose "wifi network" in settings, touch the
screen while it says so at startup, or start with nothing saved. The map comes up as usual, with a box along
the bottom naming a network, `Aimless-` and four letters: join it on a
phone, and the setup page opens (or open `http://192.168.4.1/`). Choose
a network and type its password; it is tried before it is saved. The
box counts down five minutes, and a tap on it closes setup early.

Started from [M5Launcher](https://github.com/bmorcelli/Launcher) with
nothing saved, the map first copies Launcher's own saved networks, so a
password typed into Launcher is not asked for again. Open networks are
not supported.

Tiles come from Protomaps' daily builds by default. They ask that their
bucket not be hotlinked; for regular use, copy a build to storage of your
own and set "Where remote tiles come from" and "Pinned build" under
"Aimless Moving Map" in `idf.py menuconfig`.

## Maps

Protomaps basemap extracts, MVT tiles, gzip-compressed: what
`pmtiles extract` produces from a Protomaps daily build. Zoom 14 must be
in them; zoom 12 as well gives the soft overview while tiles arrive.

The card can be **FAT32 or exFAT**. On FAT32 each file is under 4 GB, so
split a large area into bands with `original/plan-extracts.py`. On exFAT
one file can be any size, including a whole planet build -- about 126 GB,
so a card of 129 GB or more, since the filesystem needs room of its own.

exFAT comes from a patched copy of ESP-IDF's FatFs that the first
`idf.py build` makes in `components/fatfs` (`cmake/exfat.cmake`, from
Defeatist). exFAT was a Microsoft patent family; Microsoft published the
specification in 2019 and committed it to the Open Invention Network's
patent non-aggression pool. If that matters to you and you would rather
not build it in, set `TAB5_NO_EXFAT` in the environment before the first
configure, or run `./tools/enable_exfat.sh --revert` to take it out
again.

## Building

ESP-IDF 5.5 or later:

```
idf.py set-target esp32p4
idf.py build flash monitor
```

Rev 2 Tabs (ST7121 panel) only; see feckless-graphics-handler.

## Tests

```
make -C test
```

The map pipeline on a synthetic archive, the grid that follows the
position, the tile cache and the order tiles are looked for in, and the
NMEA parser. Only a C compiler is needed;
`test/make_fixture.py` regenerates the archive.

## Licence

MIT. See `LICENSE`. Map data on your card is the map provider's, under
its own licence (for Protomaps builds, OpenStreetMap's ODbL).
