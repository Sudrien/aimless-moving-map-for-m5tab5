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

all: run-maptile run-nmea run-mapview run-tilesrc run-builddate run-portalweb run-launcherkey

maptiletest: maptiletest.c ../main/maptile.c ../main/maptile.h $(CORE_SRCS) fixture.pmtiles
	$(CC) $(CFLAGS) -o $@ maptiletest.c ../main/maptile.c $(CORE_SRCS) $(LDFLAGS)

run-maptile: maptiletest
	cd $(CURDIR) && ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./maptiletest

nmeatest: nmeatest.c ../main/nmea.h
	$(CC) $(CFLAGS) -o $@ nmeatest.c $(LDFLAGS)

run-nmea: nmeatest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./nmeatest

mapviewtest: mapviewtest.c ../main/mapview.c ../main/mapview.h ../main/mapset.c ../main/mapset.h ../main/maptile.c $(CORE_SRCS) fixture.pmtiles
	$(CC) $(CFLAGS) -o $@ mapviewtest.c ../main/mapview.c ../main/mapset.c ../main/maptile.c $(CORE_SRCS) $(LDFLAGS)

run-mapview: mapviewtest
	cd $(CURDIR) && ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./mapviewtest

tilesrctest: tilesrctest.c ../main/tilesrc.c ../main/tilesrc.h ../main/tilecache.c ../main/tilecache.h ../main/mapset.c ../main/maptile.c $(CORE_SRCS) fixture.pmtiles
	$(CC) $(CFLAGS) -o $@ tilesrctest.c ../main/tilesrc.c ../main/tilecache.c ../main/mapset.c ../main/maptile.c $(CORE_SRCS) $(LDFLAGS)

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

clean:
	rm -f maptiletest nmeatest mapviewtest tilesrctest builddatetest portalwebtest launcherkeytest

.PHONY: all run-maptile run-nmea run-mapview run-tilesrc run-builddate run-portalweb run-launcherkey clean
