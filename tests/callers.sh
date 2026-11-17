#!/bin/sh
# Run neuroterm under the name "xterm" with the command lines the Plebian-OS
# callers of xterm use, and check each does what the caller needs.
#
#   pleb-session        xterm -geometry 100x30 -e sh -c '...; exec $SHELL' sh ARG   (three cases)
#   kilix privilege.py  /usr/bin/xterm -T TITLE -e python3 privilege.py --result-file F HELPER TITLE
#   kilix fingerlock.py xterm -T TITLE -e sudo -- apt-get install ...
#   kilix content_app   xterm -T LABEL -e PROGRAM ARGS...
#   pleb test           xterm -title TITLE -geometry 40x10 -e sh -c 'exec sleep 600'
#   a message window    xterm -T TITLE -geometry 78x12 -e sh -c 'echo ...; read answer'
#   Debian's uxterm     xterm -class UXTerm -u8 ...
#
# Same switches as check.sh: NEUROTERM_BIN, NEUROTERM_CHECK_DISPLAY.
# With NEUROTERM_BIN=/usr/bin/xterm it checks the installed replacement.
set -u
cd "$(dirname "$0")/.."
BIN=${NEUROTERM_BIN:-$PWD/build/neuroterm}
LIVE=${NEUROTERM_CHECK_DISPLAY:-}
tools="xdotool"
[ -n "$LIVE" ] || tools="Xvfb $tools"
for tool in $tools; do
    command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool is not installed"; exit 77; }
done

D=$(mktemp -d)
fails=0
PIDS=
pass() { echo "ok    $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT
wait_for() {
    tries=0
    while [ $tries -lt 50 ]; do
        if eval "$1" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
        tries=$((tries + 1))
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

# The program under the names its callers use.
mkdir -p "$D/bin"
ln -s "$BIN" "$D/bin/xterm"
ln -s "$BIN" "$D/bin/uxterm"
X=$D/bin/xterm
export NEUROTERM_DUMP_ON_USR1="$D/dump" SHELL=/bin/sh ENV=/dev/null PS1='$ '
unset NEURONTE_HOME NEUROTERM_WEIGHTS

dump() { # dump PID
    rm -f "$D/dump"
    kill -USR1 "$1"
    wait_for "[ -s '$D/dump' ]"
}
focus() { # focus PID: under a window manager the new window must be given the keyboard
    wait_for "xdotool search --pid $1" || return 1
    if [ -n "$LIVE" ]; then
        w=$(xdotool search --pid "$1" | tail -1)
        xdotool windowactivate "$w" 2>/dev/null
        xdotool windowfocus "$w" 2>/dev/null
        sleep 0.5
    fi
}

# --- pleb-session: the three rescue cases share one command line ---
n=0
for text in 'pleb: kilix missing at: $1' 'pleb: window manager failed to start; see $1' 'pleb: kilix is crash-looping; see $1'; do
    n=$((n + 1))
    "$X" -geometry 100x30 -e sh -c "printf \"%s\\n\\n\" \"$text\"; exec \"\${SHELL:-/bin/sh}\"" sh /some/path/or/log &
    pid=$!; PIDS="$PIDS $pid"
    focus "$pid" || { fail "rescue case $n: no window"; continue; }
    sleep 0.3; dump "$pid"
    want=$(printf '%s' "$text" | sed 's#\$1#/some/path/or/log#')
    if head -1 "$D/dump" | grep -q 'cols=100 rows=30' && grep -qF "$want" "$D/dump"; then
        pass "rescue case $n: 100x30, shows \"$want\""
    else fail "rescue case $n: $(head -2 "$D/dump" | tr '\n' '|')"; fi
    xdotool type --delay 2 "echo rescued-$n > $D/rescued-$n; exit 5"
    xdotool key Return
    if wait_for "! kill -0 $pid" && grep -q "rescued-$n" "$D/rescued-$n" 2>/dev/null; then
        wait "$pid"; rc=$?
        if [ $rc -eq 0 ]; then pass "rescue case $n: the shell takes commands; ends with status 0"
        else fail "rescue case $n: status $rc"; fi
    else fail "rescue case $n: the shell did not take the command"; fi
done

# --- kilix privilege.py: the result is in a file, the terminal must return 0 ---
cat >"$D/privilege" <<'EOS'
#!/bin/sh
# stands in for: python3 privilege.py --result-file FILE HELPER TITLE
[ "$1" = --result-file ] || exit 99
( umask 077; printf '7\n' >"$2" )
printf '%s\n' "$3" "$4" >"$2.args"
sleep 1
exit 7
EOS
chmod +x "$D/privilege"
"$X" -T "Set up Waydroid" -e "$D/privilege" --result-file "$D/status" /usr/lib/helper "Set up Waydroid" &
pid=$!; PIDS="$PIDS $pid"
if wait_for "xdotool search --name '^Set up Waydroid\$'"; then pass "privilege: -T sets the window title"; else fail "privilege: title"; fi
wait "$pid"; rc=$?
if [ $rc -eq 0 ] && [ "$(cat "$D/status" 2>/dev/null)" = 7 ] && [ "$(sed -n 2p "$D/status.args")" = "Set up Waydroid" ]; then
    pass "privilege: child returned 7 in its file, the terminal returned 0, arguments intact"
else fail "privilege: rc=$rc status=$(cat "$D/status" 2>/dev/null)"; fi

# --- kilix fingerlock.py: sudo -- argv, passed through untouched ---
cat >"$D/sudo" <<EOS
#!/bin/sh
for a in "\$@"; do printf '[%s]' "\$a"; done >"$D/sudo.args"
EOS
chmod +x "$D/sudo"
"$X" -T "Install fprintd" -e "$D/sudo" -- apt-get install -y --no-install-recommends fprintd
rc=$?
if [ $rc -eq 0 ] && [ "$(cat "$D/sudo.args" 2>/dev/null)" = "[--][apt-get][install][-y][--no-install-recommends][fprintd]" ]; then
    pass "fingerlock: everything after -e reaches the command, options included"
else fail "fingerlock: rc=$rc args=$(cat "$D/sudo.args" 2>/dev/null)"; fi

# --- kilix content_app: a program with arguments, one of them with a space ---
"$X" -T "Some App" -e "$D/sudo" one "two words" -T not-a-title
rc=$?
if [ $rc -eq 0 ] && [ "$(cat "$D/sudo.args")" = "[one][two words][-T][not-a-title]" ]; then
    pass "content_app: arguments with spaces and dashes are kept"
else fail "content_app: rc=$rc args=$(cat "$D/sudo.args")"; fi

# --- pleb test: a probe window found by its title ---
"$X" -title "pleb-probe-$$" -geometry 40x10 -e sh -c 'exec sleep 600' &
pid=$!; PIDS="$PIDS $pid"
if wait_for "xdotool search --name '^pleb-probe-$$\$'"; then
    dump "$pid"
    if head -1 "$D/dump" | grep -q 'cols=40 rows=10'; then pass "pleb test probe: -title and -geometry 40x10"
    else fail "pleb test probe: $(head -1 "$D/dump")"; fi
else fail "pleb test probe: no window with that title"; fi
kill "$pid"
if wait_for "! kill -0 $pid"; then pass "pleb test probe: ends when killed"; else fail "pleb test probe: still running"; fi

# --- a message window that waits for Enter (the test guest's stand-in for a browser) ---
"$X" -T "Browser" -geometry 78x12 -e sh -c 'echo "This does not work here."; read answer' &
pid=$!; PIDS="$PIDS $pid"
focus "$pid"; sleep 0.3; dump "$pid"
if head -1 "$D/dump" | grep -q 'cols=78 rows=12' && grep -q '^This does not work here\.$' "$D/dump"; then pass "message window: 78x12 with its text"
else fail "message window: $(head -2 "$D/dump" | tr '\n' '|')"; fi
xdotool key Return
if wait_for "! kill -0 $pid"; then pass "message window: closes on Enter"; else fail "message window: still open after Enter"; fi

# --- Debian's uxterm script: xterm -class UXTerm -u8 ---
"$X" -class UXTerm -u8 -e sleep 2 2>"$D/u8.err" &
pid=$!; PIDS="$PIDS $pid"
if wait_for "xdotool search --class UXTerm"; then pass "uxterm form: window class UXTerm"; else fail "uxterm form: no window of class UXTerm"; fi
if grep -q -- '-u8 has no effect' "$D/u8.err"; then pass "uxterm form: -u8 is reported, not passed over in silence"; else fail "uxterm form: $(cat "$D/u8.err")"; fi
wait "$pid"
"$D/bin/uxterm" -e sleep 2 &
pid=$!; PIDS="$PIDS $pid"
if wait_for "xdotool search --class UXTerm"; then pass "started as uxterm: window class UXTerm"; else fail "started as uxterm: class"; fi
wait "$pid"

# --- options: -fa and -fs, -hold, and one that cannot be honoured ---
"$X" -fa Monospace -fs 14 -geometry 80x24 -e sleep 3 2>"$D/fa.err" &
pid=$!; PIDS="$PIDS $pid"
wait_for "xdotool search --pid $pid"; sleep 0.2; dump "$pid"
if head -1 "$D/dump" | grep -q 'cols=80 rows=24 cell=11x22' && grep -q -- '-fa Monospace has no effect' "$D/fa.err"; then
    pass "-fs 14 gives an 11x22 cell; -fa is reported as having no effect"
else fail "-fa/-fs: $(head -1 "$D/dump") / $(cat "$D/fa.err")"; fi
kill "$pid"; wait "$pid" 2>/dev/null

"$X" -hold -e sh -c 'echo held-output' &
pid=$!; PIDS="$PIDS $pid"
sleep 1.5
if kill -0 "$pid" 2>/dev/null && dump "$pid" && grep -q '^held-output$' "$D/dump"; then pass "-hold keeps the window and its text after the command ends"
else fail "-hold: the terminal ended"; fi
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null

bad=0
for opt in -k8 +u8 -into -xrm -no-such-option stray-word; do
    "$X" $opt -e true 2>"$D/bad.err"; rc=$?
    if [ $rc -eq 2 ] && grep -q 'is not supported' "$D/bad.err"; then :; else bad=1; fail "$opt should stop with status 2: rc=$rc $(cat "$D/bad.err")"; fi
done
[ $bad -eq 0 ] && pass "options that cannot be honoured (-k8 +u8 -into -xrm, unknown ones) stop with status 2 and a message"

ln -s "$BIN" "$D/bin/koi8rxterm"
"$D/bin/koi8rxterm" -e true 2>"$D/koi.err"; rc=$?
if [ $rc -eq 2 ] && grep -q 'KOI8-R is not supported' "$D/koi.err"; then pass "started as koi8rxterm: refuses, since it is UTF-8 only"; else fail "koi8rxterm: rc=$rc"; fi

# --- a command that cannot be run: say so and give a shell, never an empty or vanished window ---
"$X" -e "$D/no-such-program" &
pid=$!; PIDS="$PIDS $pid"
wait_for "xdotool search --pid $pid"; sleep 0.4; dump "$pid"
if grep -q 'cannot run .*no-such-program' "$D/dump" && grep -q '^\$' "$D/dump"; then pass "a command that cannot be run: the window says so and gives a shell"
else fail "missing command: $(sed -n 2,3p "$D/dump" | tr '\n' '|')"; fi
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null

echo
if [ $fails -eq 0 ]; then echo "all caller checks passed"; else echo "$fails caller check(s) failed"; fi
[ $fails -eq 0 ]
