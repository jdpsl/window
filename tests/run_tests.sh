#!/bin/bash
# run_tests.sh - automated test runner for window
# runs each test script, checks stderr for errors

cd "$(dirname "$0")/.."

if [ -z "$DISPLAY" ]; then
    echo "SKIP: no DISPLAY set"
    exit 0
fi

PASS=0
FAIL=0

run_test() {
    local name=$1
    local script=$2

    # run test, capture stderr, timeout after 5s
    ERR=$(timeout 5 bash "$script" 2>&1)
    STATUS=$?

    if [ $STATUS -eq 124 ]; then
        echo "FAIL $name (timeout)"
        FAIL=$((FAIL + 1))
        return
    fi

    # fail if any known error patterns appear in stderr
    if echo "$ERR" | grep -qiE "unknown command|cannot load|not found|error|failed"; then
        echo "FAIL $name"
        echo "$ERR" | grep -iE "unknown command|cannot load|not found|error|failed" | sed 's/^/     /'
        FAIL=$((FAIL + 1))
    else
        echo "PASS $name"
        PASS=$((PASS + 1))
    fi
}

echo "running window tests..."
echo ""

run_test "shapes"     tests/test_shapes.sh
run_test "text"       tests/test_text.sh
run_test "images"     tests/test_images.sh
run_test "transforms" tests/test_transforms.sh
run_test "cursor"     tests/test_cursor.sh

echo ""
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
