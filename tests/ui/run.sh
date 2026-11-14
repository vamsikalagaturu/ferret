#!/usr/bin/env bash
# Runs the UI scripts (tests/ui/*.txt, or the ones named) with `FERRET --replay`, each on a fresh copy of the
# fixture tree with its own HOME, XDG_CONFIG_HOME and XDG_DATA_HOME (the trash) under one scratch directory.
# Builds nothing; a sanitizer build (platform/linux/build.sh debug) also catches memory errors.
#   tests/ui/run.sh FERRET [SCRIPT...]      FR_UI_OUT=DIR: the scratch directory (default: a new one in $TMPDIR)
# Opens a window per script on the running Wayland or X11 desktop.
set -uo pipefail

if [ $# -lt 1 ] || [ ! -x "$1" ]; then
    echo "usage: $0 FERRET [SCRIPT...]" >&2
    exit 2
fi
FERRET=$(realpath "$1")
shift
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [ $# -gt 0 ]; then SCRIPTS=("$@"); else SCRIPTS=("$HERE"/*.txt); fi
OUT=${FR_UI_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/ferret-ui.XXXXXX")}
mkdir -p "$OUT"

# Folders first, then files, by name: alpha beta empty gamma; a.txt notes.txt photo.png report.tar.gz zeta.md
fixture() {
    local t=$1
    mkdir -p "$t/alpha/sub" "$t/beta" "$t/empty" "$t/gamma"
    printf 'inside alpha\n' >"$t/alpha/inner.txt"
    printf 'in sub\n' >"$t/alpha/sub/deep.txt"
    printf 'beta one\n' >"$t/beta/b1.txt"
    printf 'the other notes\n' >"$t/beta/notes.txt"
    printf 'g\n' >"$t/gamma/g.txt"
    printf 'a\n' >"$t/a.txt"
    printf 'hello notes\n' >"$t/notes.txt"
    printf '\x89PNG\r\n' >"$t/photo.png"
    printf 'tar\n' >"$t/report.tar.gz"
    printf '# zeta\n' >"$t/zeta.md"
    printf 'secret\n' >"$t/.hidden"
}

contains() { [[ $(<"$1") == *"$2"* ]]; }

pass=0
failed=()
for script in "${SCRIPTS[@]}"; do
    script=$(realpath "$script")
    name=$(basename "$script" .txt)
    dir=$OUT/$name
    if [ -e "$dir" ]; then
        echo "$name: $dir exists; use a new FR_UI_OUT" >&2
        failed+=("$name")
        continue
    fi
    mkdir -p "$dir/home" "$dir/config" "$dir/data/Trash/files" "$dir/data/Trash/info" "$dir/shots"
    fixture "$dir/tree"
    log=$dir/stderr.log
    (cd "$dir/shots" && HOME=$dir/home XDG_CONFIG_HOME=$dir/config XDG_DATA_HOME=$dir/data \
        ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1} \
        timeout 300 "$FERRET" --replay "$script" "$dir/tree" >"$dir/stdout.log" 2>"$log")
    rc=$?
    why=""
    if contains "$log" "ERROR: AddressSanitizer" || contains "$log" "ERROR: LeakSanitizer"; then why="sanitizer"
    elif contains "$log" "runtime error:"; then why="undefined behaviour"
    elif [ $rc -eq 124 ]; then why="timed out"
    elif [ $rc -ne 0 ]; then why="exit $rc"
    fi
    if [ -z "$why" ]; then
        pass=$((pass + 1))
        printf 'PASS  %s\n' "$name"
    else
        failed+=("$name")
        printf 'FAIL  %s (%s)\n' "$name" "$why"
        head -n 40 "$log" | sed 's/^/      /'
    fi
done

printf '\n%d passed, %d failed; logs and trees in %s\n' "$pass" "${#failed[@]}" "$OUT"
[ ${#failed[@]} -eq 0 ]
