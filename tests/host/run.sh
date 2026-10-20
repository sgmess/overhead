#!/bin/bash
# Host tests: the firmware's logic built for the PC, with stand-ins for
# ESP-IDF (stub/, fake.c) and AddressSanitizer and UBSan on, so a bug that
# only shows in a rare case on the device fails here instead. CI runs them;
# no board, no ESP-IDF, no network beyond fetching cJSON once if needed.
#
#   tests/host/run.sh              every test
#   tests/host/run.sh feed         just test_feed.c
#   VERBOSE=1 tests/host/run.sh    with the firmware's log lines
#
# Each test #includes the .c file it tests, so it can reach its statics.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=../..
OUT=build
mkdir -p "$OUT"

# The firmware's cJSON (managed_components, after an ESP-IDF build), or the
# same release fetched once
CJSON=${CJSON_DIR:-$ROOT/managed_components/espressif__cjson/cJSON}
if [ ! -f "$CJSON/cJSON.c" ]; then
    CJSON=$OUT/cJSON
    if [ ! -f "$CJSON/cJSON.c" ]; then
        mkdir -p "$CJSON"
        for f in cJSON.c cJSON.h; do
            curl -fsSL "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/$f" -o "$CJSON/$f"
        done
    fi
fi

CFLAGS=(-std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers
        -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer
        -Istub -I. -I$ROOT/main -I$ROOT/main/core -I$ROOT/main/sources -I$ROOT/main/device -I"$CJSON")
# Fixtures and the test's own allocations live to the end; leaks aren't what is tested
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
export VERBOSE=${VERBOSE:-0}

failed=0
for t in test_${1:-*}.c; do
    name=${t%.c}
    echo "== $name"
    gcc "${CFLAGS[@]}" -DVERBOSE_DEFAULT="$VERBOSE" "$t" fake.c $ROOT/main/core/airlines.c "$CJSON/cJSON.c" -lm \
        -o "$OUT/$name"
    "$OUT/$name" || failed=1
done
exit $failed
