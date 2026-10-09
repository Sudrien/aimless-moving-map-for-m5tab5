# Host tests. Run from anywhere:  make -C test
#
# maptiletest: components/mapcore and main/maptile.c against
# fixture.pmtiles, which make_fixture.py writes (committed, so only a C
# compiler is needed here).
#
# nmeatest: main/nmea.h, the GNSS module's sentences.
#
# mapviewtest: main/mapset.c and main/mapview.c -- the grid placed, filled,
# moved and composed into the 1280 x 720 window -- on the fixture.
#
# tilesrctest: main/tilecache.c and main/tilesrc.c -- the cache's files,
# and the order tiles are looked for in -- with the fixture as the network.
#
# builddatetest: main/builddate.h, the dates daily builds are named for.
#
# portalwebtest: main/portalweb.c and main/dnsreply.c, the setup portal's
# form and DNS -- the code that reads bytes from anyone in radio range.
#
# launcherkeytest: main/launcherkey.c, M5Launcher's saved passwords
# decrypted and its key found in a firmware image.
#
# suntest: main/sun.c, sunrise and sunset against astral's, and the
# day/night question asked of them.
#
# uirowtest: main/uirow.c, where the buttons, the pan squares, the wake
# zone and the panels are, and what a touch hits.
#
# waypointstest: main/waypoints.c, saved points and the original's file.
#
# worldtiletest: tools/fetch_worldtile.py's walk of an archive against
# mapcore's, on the fixture, and main/worldtile.c's drawing and placing.
# Needs python3 as well as the compiler.
#
# lastfixtest: main/lastfix.c, the original's /lastfix.bin.
#
# areatest: main/area.c, the area cache's walk, against the fixture.
#
# aoptest: main/ubx.c and main/aop.c, AssistNow Autonomous -- UBX frames
# picked out of an NMEA stream, captured and walked, and the original's
# /aopdb.bin.
#
# maplabeltest: main/maplabel.c, names collected from the fixture and laid
# out over the window -- rank order, collisions, the map band, UTF-8 cuts.
#
# -fno-sanitize=shift: raster.c converts to fixed point with
# `v << RS_FRAC_BITS` on signed values that can be negative (geometry
# outside the tile). That is undefined in C11 and does the arithmetic
# thing on every compiler this builds with, GCC on RISC-V included; it is
# mapcore's, unchanged from original/, so the test tolerates it rather
# than the port editing it.
#
# SPDX-License-Identifier: MIT

CC ?= cc
CORE := ../components/mapcore
INC  := -I../main -I$(CORE)/include
CFLAGS  := -std=c11 -g -O1 -Wall -Wextra -Werror \
           -fsanitize=address,undefined -fno-sanitize-recover=all \
           -fno-omit-frame-pointer -fno-sanitize=shift $(INC)
LDFLAGS := -fsanitize=address,undefined -fno-sanitize=shift -lm

CORE_SRCS := $(CORE)/src/inflate.c $(CORE)/src/mvt.c $(CORE)/src/pmtiles.c \
             $(CORE)/src/raster.c $(CORE)/src/style.c $(CORE)/src/tile_grid.c

# maptile.c collects labels into maplabel.c's sets (0029).
TILE_SRCS := ../main/maptile.c ../main/maplabel.c
TILE_DEPS := $(TILE_SRCS) ../main/maptile.h ../main/maplabel.h

all: run-maptile run-nmea run-mapview run-tilesrc run-builddate run-portalweb run-launcherkey run-sun run-uirow run-waypoints run-worldtile run-lastfix run-area run-aop run-maplabel

maptiletest: maptiletest.c $(TILE_DEPS) $(CORE_SRCS) fixture.pmtiles
	$(CC) $(CFLAGS) -o $@ maptiletest.c $(TILE_SRCS) $(CORE_SRCS) $(LDFLAGS)

run-maptile: maptiletest
	cd $(CURDIR) && ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./maptiletest

nmeatest: nmeatest.c ../main/nmea.h
	$(CC) $(CFLAGS) -o $@ nmeatest.c $(LDFLAGS)

run-nmea: nmeatest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./nmeatest

mapviewtest: mapviewtest.c ../main/mapview.c ../main/mapview.h ../main/mapset.c ../main/mapset.h $(TILE_SRCS) $(CORE_SRCS) fixture.pmtiles
	$(CC) $(CFLAGS) -o $@ mapviewtest.c ../main/mapview.c ../main/mapset.c $(TILE_SRCS) $(CORE_SRCS) $(LDFLAGS)

run-mapview: mapviewtest
	cd $(CURDIR) && ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./mapviewtest

tilesrctest: tilesrctest.c ../main/tilesrc.c ../main/tilesrc.h ../main/tilecache.c ../main/tilecache.h ../main/mapset.c $(TILE_SRCS) $(CORE_SRCS) fixture.pmtiles
	$(CC) $(CFLAGS) -o $@ tilesrctest.c ../main/tilesrc.c ../main/tilecache.c ../main/mapset.c $(TILE_SRCS) $(CORE_SRCS) $(LDFLAGS)

run-tilesrc: tilesrctest
	cd $(CURDIR) && ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./tilesrctest

builddatetest: builddatetest.c ../main/builddate.h
	$(CC) $(CFLAGS) -o $@ builddatetest.c $(LDFLAGS)

run-builddate: builddatetest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./builddatetest

portalwebtest: portalwebtest.c ../main/portalweb.c ../main/portalweb.h ../main/dnsreply.c ../main/dnsreply.h
	$(CC) $(CFLAGS) -o $@ portalwebtest.c ../main/portalweb.c ../main/dnsreply.c $(LDFLAGS)

run-portalweb: portalwebtest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./portalwebtest

launcherkeytest: launcherkeytest.c ../main/launcherkey.c ../main/launcherkey.h
	$(CC) $(CFLAGS) -o $@ launcherkeytest.c ../main/launcherkey.c $(LDFLAGS)

run-launcherkey: launcherkeytest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./launcherkeytest

suntest: suntest.c ../main/sun.c ../main/sun.h
	$(CC) $(CFLAGS) -o $@ suntest.c ../main/sun.c $(LDFLAGS)

run-sun: suntest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./suntest

uirowtest: uirowtest.c ../main/uirow.c ../main/uirow.h
	$(CC) $(CFLAGS) -o $@ uirowtest.c ../main/uirow.c $(LDFLAGS)

run-uirow: uirowtest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./uirowtest

waypointstest: waypointstest.c ../main/waypoints.c ../main/waypoints.h
	$(CC) $(CFLAGS) -o $@ waypointstest.c ../main/waypoints.c $(LDFLAGS)

run-waypoints: waypointstest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./waypointstest

worldtiletest: worldtiletest.c ../main/worldtile.c ../main/worldtile.h $(TILE_SRCS) $(CORE_SRCS) fixture.pmtiles
	$(CC) $(CFLAGS) -o $@ worldtiletest.c ../main/worldtile.c $(TILE_SRCS) $(CORE_SRCS) $(LDFLAGS)

run-worldtile: worldtiletest ../tools/fetch_worldtile.py
	cd $(CURDIR) && python3 -I ../tools/fetch_worldtile.py worldtile.bin --file fixture.pmtiles --zxy 14/4823/6160
	cd $(CURDIR) && ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./worldtiletest worldtile.bin

lastfixtest: lastfixtest.c ../main/lastfix.c ../main/lastfix.h
	$(CC) $(CFLAGS) -o $@ lastfixtest.c ../main/lastfix.c $(LDFLAGS)

run-lastfix: lastfixtest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./lastfixtest

areatest: areatest.c ../main/area.c ../main/area.h ../main/mapset.c $(TILE_SRCS) $(CORE_SRCS) fixture.pmtiles
	$(CC) $(CFLAGS) -o $@ areatest.c ../main/area.c ../main/mapset.c $(TILE_SRCS) $(CORE_SRCS) $(LDFLAGS)

run-area: areatest
	cd $(CURDIR) && ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./areatest

aoptest: aoptest.c ../main/ubx.c ../main/ubx.h ../main/aop.c ../main/aop.h
	$(CC) $(CFLAGS) -o $@ aoptest.c ../main/ubx.c ../main/aop.c $(LDFLAGS)

run-aop: aoptest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./aoptest

maplabeltest: maplabeltest.c ../main/mapview.c ../main/mapview.h ../main/mapset.c $(TILE_DEPS) $(CORE_SRCS) fixture.pmtiles
	$(CC) $(CFLAGS) -o $@ maplabeltest.c ../main/mapview.c ../main/mapset.c $(TILE_SRCS) $(CORE_SRCS) $(LDFLAGS)

run-maplabel: maplabeltest
	cd $(CURDIR) && ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./maplabeltest

clean:
	rm -f maplabeltest areatest lastfixtest maptiletest nmeatest mapviewtest tilesrctest builddatetest portalwebtest launcherkeytest suntest uirowtest waypointstest worldtiletest worldtile.bin aoptest

.PHONY: all run-maptile run-nmea run-mapview run-tilesrc run-builddate run-portalweb run-launcherkey run-sun run-uirow run-waypoints run-worldtile run-lastfix run-area run-aop run-maplabel clean
