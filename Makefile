# neuroterm: a small X11 terminal drawn by Neuronte.
#
#   make NEURONTE_HOME=/path/to/neuronte-v2
#
# NEURONTE_HOME is a Neuronte v2 source tree (src/, include/, weights/).
# Its newest neuronte-split-vN.nrt and every neuronte-term-*.nrt in WEIGHTS
# directories are put inside the program. EXTRA_WEIGHTS names more
# directories of bundles to put in.

CC ?= cc
CFLAGS ?= -O2
WARN = -Wall -Wextra
X11_CFLAGS ?= $(shell pkg-config --cflags x11 2>/dev/null)
X11_LIBS ?= $(shell pkg-config --libs x11 2>/dev/null || echo -lX11)

ifeq ($(strip $(NEURONTE_HOME)),)
ifeq ($(filter clean,$(MAKECMDGOALS)),)
$(error set NEURONTE_HOME to a Neuronte v2 source tree: make NEURONTE_HOME=/path/to/neuronte-v2)
endif
endif

WEIGHTS = $(NEURONTE_HOME)/weights $(EXTRA_WEIGHTS)
VT = third_party/libvterm
VT_SRC = encoding keyboard mouse parser pen screen state unicode vterm
OBJS = build/neuroterm.o build/glyphs.o build/embed.o build/weights.o \
       build/nrt-neuronte.o build/nrt-utf8.o $(VT_SRC:%=build/vt-%.o)

all: build/neuroterm

build:
	mkdir -p build

build/neuroterm: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(X11_LIBS) -lm

build/neuroterm.o: neuroterm.c glyphs.h | build
	$(CC) -std=c11 $(WARN) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) -I$(VT)/include -c -o $@ $<

build/glyphs.o: glyphs.c glyphs.h | build
	$(CC) -std=c11 $(WARN) $(CPPFLAGS) $(CFLAGS) -I$(NEURONTE_HOME)/include -c -o $@ $<

# Neuronte itself, built as its own Makefile builds it.
build/nrt-%.o: $(NEURONTE_HOME)/src/%.c | build
	$(CC) -std=c11 -O3 -I$(NEURONTE_HOME)/include -c -o $@ $<

build/vt-%.o: $(VT)/src/%.c | build
	$(CC) -std=c99 $(CPPFLAGS) $(CFLAGS) -I$(VT)/include -c -o $@ $<

# The weight files, inside the program.
build/weights.S build/embed.c: tools/embed.sh $(wildcard $(WEIGHTS:%=%/neuronte-*.nrt)) | build
	sh tools/embed.sh build/weights.S build/embed.c $(WEIGHTS)

build/weights.o: build/weights.S
	$(CC) -c -o $@ $<

build/embed.o: build/embed.c
	$(CC) -std=c11 -c -o $@ $<

check: build/neuroterm
	sh tests/check.sh

clean:
	rm -rf build

.PHONY: all check clean
