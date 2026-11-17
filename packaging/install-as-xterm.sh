#!/bin/sh
# Put neuroterm in the place of xterm on a Debian system, and take it out again.
#
#   install-as-xterm.sh install /path/to/neuroterm
#   install-as-xterm.sh remove
#   install-as-xterm.sh status
#
# install copies the program to /usr/local/bin/neuroterm and makes
# /usr/bin/xterm and /usr/bin/uxterm links to it. The xterm package's own
# two files are moved aside with dpkg-divert (to xterm.distrib and
# uxterm.distrib), so the package stays installed, anything that depends on
# it stays satisfied, and an upgrade or reinstall of xterm writes to the
# moved-aside names and cannot put the old program back under its name.
# Programs that run "xterm" or "uxterm" get neuroterm with no change to them.
#
# remove takes the links away and gives the package its files back.
# Run as root. ROOT=/some/dir works on a tree instead of / (for tests).
set -eu
ROOT=${ROOT:-}
NAMES="xterm uxterm"
TARGET=/usr/local/bin/neuroterm

divert() { dpkg-divert ${ROOT:+--root="$ROOT"} "$@"; }

case ${1:-} in
install)
    src=${2:?"give the path of the built neuroterm"}
    [ -x "$src" ] || { echo "install-as-xterm: $src is not an executable file" >&2; exit 1; }
    mkdir -p "$ROOT/usr/local/bin" "$ROOT/usr/bin"
    cp "$src" "$ROOT$TARGET.new"
    chmod 755 "$ROOT$TARGET.new"
    mv "$ROOT$TARGET.new" "$ROOT$TARGET"
    for name in $NAMES; do
        # A link of ours from an earlier run is not the package's file: take
        # it away first, or dpkg-divert would move it aside over the real one.
        if [ -L "$ROOT/usr/bin/$name" ] && [ "$(readlink "$ROOT/usr/bin/$name")" = "$TARGET" ]; then
            rm -f "$ROOT/usr/bin/$name"
        fi
        divert --local --rename --divert "/usr/bin/$name.distrib" --add "/usr/bin/$name"
        ln -s "$TARGET" "$ROOT/usr/bin/$name"
    done
    echo "installed: xterm and uxterm are now neuroterm"
    ;;
remove)
    for name in $NAMES; do
        if [ -L "$ROOT/usr/bin/$name" ] && [ "$(readlink "$ROOT/usr/bin/$name")" = "$TARGET" ]; then
            rm -f "$ROOT/usr/bin/$name"
        fi
        divert --local --rename --remove "/usr/bin/$name"
    done
    echo "removed: xterm and uxterm are the package's own again (if it is installed)"
    echo "$TARGET is left in place; delete it by hand if it is not wanted"
    ;;
status)
    for name in $NAMES; do
        f=$ROOT/usr/bin/$name
        if [ -L "$f" ]; then echo "$f -> $(readlink "$f")"
        elif [ -e "$f" ]; then echo "$f is a file of its own"
        else echo "$f is missing"; fi
        divert --list "/usr/bin/$name" || true
    done
    ;;
*)
    sed -n '2,7p' "$0" >&2
    exit 2
    ;;
esac
