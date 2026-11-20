# neuroterm: a small X11 terminal drawn by Neuronte.
#
#   make NEURONTE_HOME=/path/to/neuronte-v2
#
# NEURONTE_HOME is a Neuronte v2 source tree (include/, weights/, and a
# build of libneuronte). The program links that library and neuronte-sdk.
# It does not compile neuronte.c. The newest neuronte-split-vN.nrt, every
# neuronte-term-*.nrt, and neuronte-emoji-vN in the WEIGHTS directories are
# put inside the program, along with any Metric Sans faces. EXTRA_WEIGHTS
# names more directories of bundles to put in.

.DEFAULT_GOAL := all

CC ?= cc
CFLAGS ?= -O2
WARN = -Wall -Wextra
X11_CFLAGS ?= $(shell pkg-config --cflags x11 2>/dev/null)
X11_LIBS ?= $(shell pkg-config --libs x11 2>/dev/null || echo -lX11)

ifeq ($(strip $(NEURONTE_HOME)),)
ifeq ($(filter clean check-embed,$(MAKECMDGOALS)),)
$(error set NEURONTE_HOME to a Neuronte v2 source tree: make NEURONTE_HOME=/path/to/neuronte-v2)
endif
endif

NEURONTE_SDK_HOME ?= $(HOME)/gpu_terminal/kilix-modules/neuronte-sdk
SDK_MASK = $(NEURONTE_SDK_HOME)/include/neuronte_sdk_mask.h
SDK_TERMINAL = $(NEURONTE_SDK_HOME)/include/neuronte_sdk_terminal.h
SDK_BUILD := $(abspath build/neuronte-sdk)
SDK_LIB := $(SDK_BUILD)/libneuronte-sdk.a
NEURONTE_LIB := $(NEURONTE_HOME)/build/libneuronte.a

WEIGHTS = $(NEURONTE_HOME)/weights $(EXTRA_WEIGHTS)
include embed.mk
OBJS = build/neuroterm.o build/glyphs.o build/embed.o build/weights.o \
       build/nsdk-mask.o $(NEUROTERM_OBJECTS)

all: build/neuroterm

build:
	mkdir -p build

$(NEURONTE_LIB): $(NEURONTE_HOME)/src/neuronte.c $(NEURONTE_HOME)/include/neuronte.h
	$(MAKE) -C $(NEURONTE_HOME) build/libneuronte.a

$(SDK_LIB): $(NEURONTE_SDK_HOME)/src/nsdk_terminal.c $(NEURONTE_SDK_HOME)/src/neuronte_sdk.c \
		$(SDK_TERMINAL) $(NEURONTE_SDK_HOME)/Makefile $(NEURONTE_HOME)/include/neuronte.h
	$(MAKE) -C $(NEURONTE_SDK_HOME) \
		BUILD_DIR=$(SDK_BUILD) \
		NEURONTE=$(abspath $(NEURONTE_HOME)) \
		NEURONTE_INCLUDE=$(abspath $(NEURONTE_HOME)/include) \
		$(SDK_LIB)

build/neuroterm: $(OBJS) $(SDK_LIB) $(NEURONTE_LIB)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(SDK_LIB) $(NEURONTE_LIB) $(X11_LIBS) -lm

build/neuroterm.o: neuroterm.c glyphs.h include/neuroterm_terminal.h | build
	$(CC) -std=c11 $(WARN) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) $(NEUROTERM_CPPFLAGS) -c -o $@ $<

build/glyphs.o: glyphs.c glyphs.h $(SDK_MASK) $(SDK_TERMINAL) $(NEURONTE_HOME)/include/neuronte.h | build
	$(CC) -std=c11 $(WARN) $(CPPFLAGS) $(CFLAGS) -I$(NEURONTE_HOME)/include -I$(NEURONTE_SDK_HOME)/include -c -o $@ $<

build/nsdk-mask.o: $(NEURONTE_SDK_HOME)/src/neuronte_sdk_mask.c $(SDK_MASK) $(NEURONTE_HOME)/include/neuronte.h | build
	$(CC) -std=c11 $(WARN) $(CPPFLAGS) $(CFLAGS) -I$(NEURONTE_HOME)/include -I$(NEURONTE_SDK_HOME)/include -c -o $@ $<

# The weight files, inside the program.
build/weights.S build/embed.c &: tools/embed.sh $(wildcard $(WEIGHTS:%=%/neuronte-*.nrt)) | build
	sh tools/embed.sh build/weights.S build/embed.c $(WEIGHTS)

build/weights.o: build/weights.S
	$(CC) -c -o $@ $<

build/embed.o: build/embed.c
	$(CC) -std=c11 -c -o $@ $<

build/check-embed: tests/check-embed.c $(NEUROTERM_OBJECTS) | build
	$(CC) -std=c11 $(WARN) $(CPPFLAGS) $(NEUROTERM_CPPFLAGS) $(CFLAGS) -o $@ $^

check-embed: build/check-embed
	./build/check-embed

check: build/neuroterm check-embed
	sh tests/check.sh

clean:
	rm -rf build

.PHONY: all check check-embed clean
