# neuroterm

A small X11 terminal in which every character is drawn by
[Neuronte](https://github.com/itsmygithubacct/neuronte-v2), a neural net that is its own font. No font
file is opened and the X server is never asked for a font.

It is meant as a **rescue terminal**: the window that still opens when the main terminal does not. So it
is one program with few parts:

- plain Xlib (no toolkit, no fontconfig, no FreeType, no OpenGL, no input method);
- a pty and the libvterm parser (compiled in);
- the Neuronte nets (compiled in) and the weight files (inside the program);
- no configuration file, no X resources.

At run time it needs libc, libm and libX11.

`neuroterm` is a working name.

## Build

```sh
make NEURONTE_HOME=/path/to/neuronte-v2          # a Neuronte v2 source tree, tag v2.2.0 or later
make NEURONTE_HOME=... EXTRA_WEIGHTS=/dir/of/more/bundles
make check NEURONTE_HOME=...                     # needs Xvfb, xdotool, xclip; strace for one check
sh tests/callers.sh                              # the command lines of xterm's callers
```

The build needs a C compiler, the libX11 headers, and the Neuronte SDK source
with its [cell-mask API](https://github.com/itsmygithubacct/neuronte-sdk/tree/61df4489756cde5b486f1c7d66135f2bc4ea5131).
In the Kilix source tree the SDK is picked up from
`../../kilix-modules/neuronte-sdk`. Set `NEURONTE_SDK_HOME` for another checkout. It puts the newest `neuronte-split-vN.nrt` and every
`neuronte-term-*.nrt` it is given inside the program; `build/neuroterm -list-weights` names them.

## Run

```sh
build/neuroterm                                   # the shell
build/neuroterm -geometry 100x30 -T "title" -e command args...
```

It can stand in for `xterm` on a command line. Options, in three groups:

- **Honoured:** `-e`, `-T`/`-title`, `-geometry`, `-fs`, `-fg`, `-bg`, `-rv`, `-sl`, `-hold`, `-ls`,
  `-tn`, `-class`, `-name`, `-display`. `-fs` is a size in points; the cell is 0.8 by 1.6 of it in pixels.
- **Accepted and reported on stderr as having no effect:** xterm options that only choose a look that
  does not exist here: fonts (`-fa`, `-fn`, `-fb`, ...), border and scrollbar (`-bw`, `-sb`, ...),
  cursor and pointer colours, and `-u8` (it is always UTF-8).
- **Everything else stops the program** with a message and status 2 (`+u8`, `-k8`, `-into`, `-xrm`, an
  unknown option, a stray word). An option that would change what the terminal does is never passed
  over in silence. Started under the name `koi8rxterm` it refuses for the same reason.

Like xterm it exits with status 0 when its command ends, whatever the command returned. If the command
cannot be run it says so in the window and starts a shell. Started under the name `xterm` it sets the
window class `XTerm`; under `uxterm` or `lxterm`, `UXTerm`.

`packaging/install-as-xterm.sh` puts it in the place of `/usr/bin/xterm` and `/usr/bin/uxterm` on a
Debian system with `dpkg-divert`, and takes it out again. `tests/callers.sh` runs it under the name
`xterm` with the command lines Plebian-OS uses.

`TERM` is `xterm-256color`.

| Key | Action |
| --- | --- |
| Shift+PageUp, Shift+PageDown, the wheel | scroll back (10,000 lines by default) |
| drag, double click, triple click | select characters, a word, a line; the selection is the primary selection |
| middle button, Shift+Insert | paste the primary selection |
| Ctrl+Shift+C, Ctrl+Shift+V | copy to and paste from the clipboard |
| Ctrl+plus, Ctrl+minus | larger and smaller text |
| Shift with the mouse | select even when the program in the terminal has taken the mouse |

In a full-screen program (a pager, an editor) the wheel sends arrow keys.

## Neuronte Sans Metric

Build with a Neuronte source tree that supports the Metric Sans bundles.
The build embeds the Metric Sans faces from that tree’s weights directory.
Select the font before any `-e` command. You can also point
`NEUROTERM_WEIGHTS` at a directory containing the bundles.

```sh
make -B NEURONTE_HOME=/path/to/metric-sans
build/neuroterm --font metric-sans
```

The terminal keeps its fixed-width grid. It picks the Metric Sans face for
the cell size and style where available, falling back to its regular face with
synthetic styling or to the standard terminal bundles for other characters.
Use `--font metric-sans -which U+0041` to see which file draws a character.

## How a cell is drawn

The cell is 0.6 em wide and twice as high (10 x 20 pixels at the default size). For each character the
program asks `neuronte_coverage_styled()` for the coverage of the cell, a number from 0 to 1 per pixel,
keeps it as one byte per pixel, and blends the cell's colour over its background by that coverage. Bold
and italic are Neuronte's own synthetic styles. Nothing is taken from drawn pixels. The finished window
is sent with `XPutImage`.

Weight files are asked in order: the split file first, then each term bundle by name. A character goes to
the first file trained on it. A character no file was trained on is drawn as Neuronte's own mark (a
hollow box with a small square), one mark per character, across both cells when the character is two
cells wide. `build/neuroterm -which U+4E2D U+41` says which file draws a character.

A weights directory may add files the program does not hold (a newer split file, more term bundles):
`$NEUROTERM_WEIGHTS`, else `$NEURONTE_HOME/weights`, else
`~/.local/gpu_terminal/sources/neuronte/weights`, else `/usr/share/neuronte/weights`. A file there never
replaces a built-in file of the same name, and one that does not open is skipped. `-no-disk-weights`
turns this off.

## Embedding a terminal

Include `embed.mk` in the host Makefile, set `NEUROTERM_BUILD_DIR` to the
host's build directory for terminal objects, and link `NEUROTERM_OBJECTS`.
Use `NEUROTERM_CPPFLAGS` when compiling against `neuroterm_terminal.h`.
The embedded core builds without X11 or weights. The host supplies PTY
transport and its canvas and glyph backend.

Use `neuroterm_init` for an owned live screen, then call `neuroterm_feed` and
`neuroterm_read` to exchange bytes with the PTY. `neuroterm_paint_cell` shares
the terminal's colors, wide cells, glyph styling, decorations, and cursor
drawing with the X11 frontend. The host controls positioning and passes fill
and glyph callbacks. The X11 app still uses its own scrollback, selection,
and Neuronte font configuration.

Run `make check-embed` for the headless checks.

## No font: the checks

`make check` runs the terminal on a private Xvfb display with a fontconfig that knows no font and an
empty home directory, and checks:

- `strace` shows no font, fontconfig, locale or weight file opened;
- the program links no X font or text call (`XLoadFont`, `XDrawString`, `XCreateFontCursor`, ...), no
  input method, no Xft, fontconfig or FreeType;
- typing (Latin-1, Cyrillic and Greek keys too), Ctrl+C, resize, scrollback, paste (a small one and
  one of 1.3 MB), copy, the alternate screen and exit work.

## Not there yet

- Input methods, dead keys and Compose (they need locale files). Keys that give a character directly
  work in any layout, the Cyrillic and Greek ones included.
- Lines are not rewrapped when the window gets narrower or wider.
- Combining marks are drawn over their base character, not placed. No right-to-left layout.
- A drawing two cells wide is the one-cell drawing centred; emoji from Neuronte's colour file are not used.
- No blinking, no double-size lines, no OSC 52, no change of colours by escape sequence.
- The mouse pointer is whatever the parent window has (asking for the usual text pointer would mean
  asking the X server for its cursor font).

## Licences

neuroterm is MIT (`LICENSE`). libvterm in `third_party/libvterm` is MIT (its `LICENSE`; origin and the
one local patch are in `UPSTREAM`). Neuronte is MIT and is not copied here: the build reads it from
`NEURONTE_HOME`, and the built program contains its code and weight files, so ship Neuronte's `LICENSE`
with the binary.
