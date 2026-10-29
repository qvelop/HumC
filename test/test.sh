#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ -n "${HUMC_SOURCE_DIR:-}" ]; then
    ROOT=$(CDPATH= cd -- "$HUMC_SOURCE_DIR" && pwd)
elif [ -f "$SCRIPT_DIR/../src/compiler.c" ]; then
    ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
elif [ -f "$SCRIPT_DIR/src/compiler.c" ]; then
    ROOT=$SCRIPT_DIR
else
    echo "Cannot locate HumC project root." >&2
    exit 1
fi

if [ -n "${HUMC:-}" ]; then
    HUMC=$HUMC
elif [ -z "${HUMC:-}" ]; then
    if [ -x "$ROOT/build/humc" ]; then
        HUMC="$ROOT/build/humc"
    else
        HUMC="$ROOT/humc"
    fi
fi

CC=${CC:-cc}
TMP=${TMPDIR:-/tmp}/humc-smoke-$$

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"

[ -x "$HUMC" ] || fail "HumC executable not found: $HUMC"

run_program() {
    name=$1
    source=$2
    expected=$3

    c_file="$TMP/$name.c"
    executable="$TMP/$name"

    "$HUMC" "$source" -o "$c_file" >/dev/null ||
        fail "$name: HumC compilation failed"

    "$CC" -std=c11 -Wall -Wextra -Wpedantic \
        "$c_file" -o "$executable" ||
        fail "$name: C compilation failed"

    actual=$("$executable") ||
        fail "$name: program exited unsuccessfully"

    if [ "$actual" != "$expected" ]; then
        printf 'FAIL: %s output mismatch\nExpected:\n%s\nActual:\n%s\n' \
            "$name" "$expected" "$actual" >&2
        exit 1
    fi

    "$HUMC" check "$source" >/dev/null 2>&1 ||
        fail "$name: check failed"

    printf 'OK: %s\n' "$name"
}

expect_runtime_failure() {
    name=$1
    source=$2
    message=$3

    c_file="$TMP/$name.c"
    executable="$TMP/$name"

    "$HUMC" "$source" -o "$c_file" >/dev/null ||
        fail "$name: translation failed"

    "$CC" -std=c11 -Wall -Wextra -Wpedantic \
        "$c_file" -o "$executable" ||
        fail "$name: C compilation failed"

    if "$executable" >"$TMP/$name.out" 2>"$TMP/$name.err"; then
        fail "$name: expected runtime failure"
    fi

    if ! grep -Fq "$message" "$TMP/$name.err"; then
        cat "$TMP/$name.err" >&2
        fail "$name: expected error message not found"
    fi

    printf 'OK: %s\n' "$name"
}

expect_check_failure() {
    name=$1
    source=$2
    message=$3

    if "$HUMC" check "$source" >"$TMP/$name.log" 2>&1; then
        fail "$name: expected check failure"
    fi

    if ! grep -Fq "$message" "$TMP/$name.log"; then
        cat "$TMP/$name.log" >&2
        fail "$name: expected diagnostic not found"
    fi

    printf 'OK: %s\n' "$name"
}

cat >"$TMP/hello.humc" <<'HUMC'
let answer = 40 + 2
print answer
print "Hello from HumC!"
HUMC

run_program hello "$TMP/hello.humc" "$(printf '42\nHello from HumC!')"

run_program supported "$ROOT/examples/supported.humc" \
    "$(printf 'Hello from HumC\n42\nanswer is greater than 40\n0\n1\n2')"

run_program basic "$ROOT/examples/basic.humc" \
    "$(printf 'Welcome to Humal!\nBob\n25\nWarsaw\n15\n5\n50\n2\nAdult\nGood\n0\n1\n2\n3\n4\n0\n1\n2\n3\n4\n1\n2\n4\n5\nHello Bob\nHello Alex\n30')"

cat >"$TMP/array.humc" <<'HUMC'
let values = [10, 20, 30]
let indices = [1, 2]
let index = 1

print values[index]
print values[indices[0]]

values[index] = 99
print values[1]
HUMC

run_program array "$TMP/array.humc" "$(printf '20\n20\n99')"

cat >"$TMP/oob-read.humc" <<'HUMC'
let values = [10, 20, 30]
print values[4]
HUMC

expect_runtime_failure oob-read "$TMP/oob-read.humc" \
    "HumC bounds error: index 4 out of bounds"

cat >"$TMP/oob-write.humc" <<'HUMC'
let values = [10, 20, 30]
values[3] = 99
print values[0]
HUMC

expect_runtime_failure oob-write "$TMP/oob-write.humc" \
    "HumC bounds error: index 3 out of bounds"

cat >"$TMP/oob-negative.humc" <<'HUMC'
let values = [10, 20, 30]
print values[-1]
HUMC

expect_runtime_failure oob-negative "$TMP/oob-negative.humc" \
    "HumC bounds error: index -1 out of bounds"

cat >"$TMP/duplicate.humc" <<'HUMC'
let value = 1
let value = 2
print value
HUMC

expect_check_failure duplicate "$TMP/duplicate.humc" \
    "already declared in this scope"

cat >"$TMP/uninitialized.humc" <<'HUMC'
let value: int
print value
HUMC

expect_check_failure uninitialized "$TMP/uninitialized.humc" \
    "requires every variable to be initialized"

cat >"$TMP/pointer-index.humc" <<'HUMC'
fn first(int *ptr)
    print ptr[0]
HUMC

expect_check_failure pointer-index "$TMP/pointer-index.humc" \
    "length is unknown"

cat >"$TMP/branch-scope.humc" <<'HUMC'
if true
    let branch_value = 1
    print branch_value
else
    let branch_value = 2
    print branch_value

print 3
HUMC

run_program branch-scope "$TMP/branch-scope.humc" "$(printf '1\n3')"

printf '\nAll test passed ✓\n'