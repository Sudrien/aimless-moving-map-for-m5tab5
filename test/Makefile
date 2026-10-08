# Host tests. Run from anywhere:  make -C test
#
# maptiletest: components/mapcore and main/maptile.c against
# fixture.pmtiles, which make_fixture.py writes (committed, so only a C
# compiler is needed here).
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

all: run-maptile

maptiletest: maptiletest.c ../main/maptile.c ../main/maptile.h $(CORE_SRCS) fixture.pmtiles
	$(CC) $(CFLAGS) -o $@ maptiletest.c ../main/maptile.c $(CORE_SRCS) $(LDFLAGS)

run-maptile: maptiletest
	cd $(CURDIR) && ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./maptiletest

clean:
	rm -f maptiletest

.PHONY: all run-maptile clean
