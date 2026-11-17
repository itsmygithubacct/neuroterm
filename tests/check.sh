#!/bin/sh
# Drive build/neuroterm on a private Xvfb display and check that it works
# as a terminal: typing, resize, scrollback, paste, copy, exit, and that it
# opens no font file. Needs Xvfb, xdotool and xclip; strace for the file check.
#
#   NEUROTERM_BIN=/path/to/neuroterm   check an installed program instead
#   NEUROTERM_CHECK_DISPLAY=:0         use that display, not a private Xvfb.
#       The check then types and clicks in that session for about half a
#       minute: do not run it on a display somebody is using.
set -u
cd "$(dirname "$0")/.."
BIN=${NEUROTERM_BIN:-$PWD/build/neuroterm}
LIVE=${NEUROTERM_CHECK_DISPLAY:-}
tools="xdotool xclip"
[ -n "$LIVE" ] || tools="Xvfb $tools"
for tool in $tools; do
    command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool is not installed"; exit 77; }
done

D=$(mktemp -d)
fails=0
pass() { echo "ok    $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }
cleanup() {
    [ -n "${TERM_PID:-}" ] && kill "$TERM_PID" 2>/dev/null
    [ -n "${CLIP_PID:-}" ] && kill "$CLIP_PID" 2>/dev/null
    [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT

# wait_for 'shell test' : up to five seconds
wait_for() {
    n=0
    while [ $n -lt 50 ]; do
        if eval "$1" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
        n=$((n + 1))
    done
    return 1
}

if [ -n "$LIVE" ]; then
    DISPLAY=$LIVE
    XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority}
    export DISPLAY XAUTHORITY
else
    mkfifo "$D/display"
    Xvfb -displayfd 3 -screen 0 1600x1000x24 -nolisten tcp -noreset 3>"$D/display" 2>"$D/xvfb.log" &
    XVFB_PID=$!
    read -r num <"$D/display"
    DISPLAY=:$num
    export DISPLAY
    unset XAUTHORITY
fi

# A fontconfig that knows no font, and a home with nothing in it.
mkdir -p "$D/home" "$D/fc"
printf '<?xml version="1.0"?>\n<fontconfig></fontconfig>\n' >"$D/fc/fonts.conf"
export FONTCONFIG_FILE="$D/fc/fonts.conf" FONTCONFIG_PATH="$D/fc" HOME="$D/home"
unset NEURONTE_HOME NEUROTERM_WEIGHTS

dump() {
    rm -f "$D/dump"
    kill -USR1 "$TERM_PID"
    wait_for "[ -s '$D/dump' ]"
}
type_line() {
    xdotool type --delay 2 "$1"
    xdotool key Return
}

ENV=/dev/null PS1='$ ' "$BIN" -geometry 80x24 -T neuroterm-check -dump-on-usr1 "$D/dump" -e /bin/sh &
TERM_PID=$!
wait_for "xdotool search --name '^neuroterm-check\$'" || fail "the window appears"
WID=$(xdotool search --name '^neuroterm-check$' | head -1)
if [ -n "$LIVE" ]; then # a window manager decides who has the keyboard
    xdotool windowactivate "$WID" 2>/dev/null
    xdotool windowfocus "$WID" 2>/dev/null
    sleep 0.5
fi

# 1. typing reaches the shell
type_line "echo typed-\$((6*7)) > $D/typed"
if wait_for "grep -q typed-42 '$D/typed'"; then pass "typing reaches the shell"; else fail "typing reaches the shell"; fi

# 2. output reaches the screen
type_line "echo MARK-\$((100+23))"
sleep 0.3; dump
if grep -q '^MARK-123$' "$D/dump"; then pass "output reaches the screen"; else fail "output reaches the screen"; fi
if head -1 "$D/dump" | grep -q 'cols=80 rows=24'; then pass "-geometry 80x24"; else fail "-geometry 80x24: $(head -1 "$D/dump")"; fi

# 3. control keys: Ctrl+C interrupts, Ctrl+D ends input
type_line "sleep 30; echo not-interrupted > $D/intr"
sleep 0.3; xdotool key ctrl+c
type_line "echo after-interrupt > $D/intr2"
if wait_for "[ -s '$D/intr2' ]" && [ ! -e "$D/intr" ]; then pass "Ctrl+C interrupts"; else fail "Ctrl+C interrupts"; fi

# 4. resize: the child learns the new size
cw=$(head -1 "$D/dump" | sed 's/.*cell=\([0-9]*\)x.*/\1/'); chh=$((cw * 2))
xdotool windowsize "$WID" $((4 + 100 * cw)) $((4 + 30 * chh))
sleep 0.5
type_line "stty size > $D/size"
if wait_for "grep -q '^30 100\$' '$D/size'"; then pass "resize to 100x30 reaches the child"; else fail "resize: $(cat "$D/size" 2>/dev/null)"; fi

# 5. scrollback
type_line "seq 1 300"
sleep 0.5; dump
if grep -q '^300$' "$D/dump" && ! grep -q '^250$' "$D/dump"; then pass "300 lines scrolled past"; else fail "300 lines scrolled past"; fi
xdotool key shift+Prior shift+Prior
sleep 0.3; dump
if grep -q '^230$' "$D/dump" && ! grep -q '^300$' "$D/dump" && head -1 "$D/dump" | grep -q 'scroll=58 '; then
    pass "Shift+PageUp shows older lines"
else fail "Shift+PageUp: $(head -1 "$D/dump")"; fi
xdotool click 5 click 5
sleep 0.3; dump
if head -1 "$D/dump" | grep -q 'scroll=52 '; then pass "the wheel scrolls three lines"; else fail "wheel: $(head -1 "$D/dump")"; fi
xdotool key space BackSpace
sleep 0.3; dump
if head -1 "$D/dump" | grep -q 'scroll=0 ' && grep -q '^300$' "$D/dump"; then pass "a key returns to the end"; else fail "a key returns to the end"; fi

# 6. paste (Shift+Insert takes the primary selection; newlines and tabs kept, escapes dropped)
printf 'pasted one\npasted\ttwo \033[31m' | xclip -selection primary -loops 0 &
CLIP_PID=$!
sleep 0.3
type_line "cat > $D/pasted"
xdotool key shift+Insert
sleep 0.3
xdotool key Return ctrl+d
printf 'pasted one\npasted\ttwo [31m\n' >"$D/pasted.want"
if wait_for "cmp -s '$D/pasted' '$D/pasted.want'"; then pass "paste"; else fail "paste: $(od -c "$D/pasted" 2>/dev/null | head -3)"; fi
kill "$CLIP_PID" 2>/dev/null; CLIP_PID=

# 6b. a paste too large for one property (sent piece by piece)
i=0; : >"$D/big.want"
while [ $i -lt 22000 ]; do printf 'line %05d of a large paste, sixty characters wide in all.....\n' $i; i=$((i + 1)); done >"$D/big.want"
xclip -selection primary -loops 0 <"$D/big.want" &
CLIP_PID=$!
sleep 0.3
type_line "stty -echo; cat > $D/big; stty echo"
xdotool key shift+Insert
sleep 3
xdotool key ctrl+d
dump
if wait_for "cmp -s '$D/big' '$D/big.want'" && head -1 "$D/dump" | grep -q 'piecewise-pastes=1'; then pass "a 1.3 MB paste, sent piece by piece, arrives whole"; else fail "large paste: $(wc -c <"$D/big" 2>/dev/null) of $(wc -c <"$D/big.want") bytes; $(head -1 "$D/dump")"; fi
kill "$CLIP_PID" 2>/dev/null; CLIP_PID=

# 6c. keys of other alphabets: Latin-1, the Unicode range, the older Cyrillic and Greek sets
type_line "cat > $D/keys"
xdotool key eacute EuroSign Cyrillic_zhe Greek_lambda U2192
xdotool key Return ctrl+d
printf '\303\251\342\202\254\320\266\316\273\342\206\222\n' >"$D/keys.want"
if wait_for "cmp -s '$D/keys' '$D/keys.want'"; then pass "keys: e-acute, euro, Cyrillic, Greek, an arrow"; else fail "keys: $(od -c "$D/keys" 2>/dev/null | head -2)"; fi

# 7. copy: a double click selects a word and owns the primary selection
type_line "clear; echo COPY-ME-456 and more"
sleep 0.3
xdotool mousemove --window "$WID" $((2 + 3 * cw)) $((2 + chh / 2)) click --repeat 2 --delay 60 1
sleep 0.3
got=$(xclip -o -selection primary 2>/dev/null)
if [ "$got" = "COPY-ME-456" ]; then pass "double click copies a word"
else # selection=0 in the header: the clicks never reached the window; 1: it selected but another program holds the selection
    dump; fail "double click copied: '$(printf '%s' "$got" | cut -c1-40)' ($(head -1 "$D/dump" | grep -o 'selection=[01]'); owner: $(xdotool getwindowname "$(xdotool getactivewindow 2>/dev/null)" 2>/dev/null))"
fi
sleep 0.6 # so the next click starts a new count
xdotool mousemove --window "$WID" $((2 + 3 * cw)) $((2 + chh / 2)) click --repeat 3 --delay 60 1
sleep 0.3
got=$(xclip -o -selection primary 2>/dev/null)
if [ "$got" = "COPY-ME-456 and more" ]; then pass "triple click copies the line"
else dump; fail "triple click copied: '$(printf '%s' "$got" | cut -c1-40)' ($(head -1 "$D/dump" | grep -o 'selection=[01]'))"; fi

# 8. a full-screen program: alternate screen and back
type_line "printf '\\033[?1049h\\033[2J\\033[HALT-SCREEN'; sleep 1; printf '\\033[?1049l'"
sleep 0.4; dump
if grep -q '^ALT-SCREEN' "$D/dump" && ! grep -q 'COPY-ME' "$D/dump"; then pass "alternate screen"; else fail "alternate screen"; fi
sleep 1; dump
if grep -q 'COPY-ME-456' "$D/dump"; then pass "back from the alternate screen"; else fail "back from the alternate screen"; fi

# 9. the shell ends, the terminal ends, status 0
type_line "exit 3"
if wait_for "! kill -0 $TERM_PID"; then
    wait "$TERM_PID"; rc=$?
    if [ $rc -eq 0 ]; then pass "exits 0 when the shell ends (as xterm does)"; else fail "exit status $rc"; fi
else fail "the terminal ends with the shell"; fi
TERM_PID=

# 10. characters with no drawing get the mark, not another font
out=$("$BIN" -no-disk-weights -which U+41 U+4E2D U+F8FF)
if printf '%s\n' "$out" | grep -q 'U+0041.neuronte-split' && [ "$(printf '%s\n' "$out" | grep -c 'the mark')" = 2 ]; then
    pass "A is drawn; a Chinese and a private-use character get the mark"
else fail "which: $out"; fi

# 11. no font file is opened, and no weight file is needed on disk
if command -v strace >/dev/null 2>&1; then
    strace -f -e trace=open,openat,openat2 -o "$D/trace" "$BIN" -no-disk-weights -e /bin/sh -c 'printf "AZ az 09 \342\224\200 \344\270\255"; sleep 0.5'
    opened=$(grep -v ENOENT "$D/trace" | grep -c 'open')
    if grep -v ENOENT "$D/trace" | grep -iE '\.(ttf|otf|ttc|otc|pcf|pfb|pfa|bdf|woff2?|psf|pf2|afm|t1|dfont|nrt)(\.gz)?"|/fonts|fontconfig|/X11/locale' >"$D/fontfiles"; then
        fail "font-like files opened: $(cat "$D/fontfiles")"
    else pass "no font, fontconfig, locale or weight file among the $opened files opened"; fi
else echo "SKIP  strace is not installed"; fi

# 12. nothing in the program can ask the X server for a font or for text
if ! command -v nm >/dev/null 2>&1; then echo "SKIP  nm is not installed"
elif nm -D --undefined-only "$BIN" | grep -E ' (XLoad|XQueryFont|XListFonts|XDrawString|XDrawImageString|XDrawText|XCreateFontCursor|XCreateGlyphCursor|XSetFont|XCreateFontSet|XOpenIM|Xutf8|Xmb|Xwc|Xft|Fc[A-Z]|FT_)' >"$D/syms"; then
    fail "font or text calls linked: $(cat "$D/syms")"
else pass "no X font, text, input-method, Xft, fontconfig or FreeType call is linked"; fi

echo
if [ $fails -eq 0 ]; then echo "all checks passed"; else echo "$fails check(s) failed"; fi
[ $fails -eq 0 ]
