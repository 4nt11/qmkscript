#!/usr/bin/env bash
# tests/run.sh -- entry point. Corre test_vm y test_compiler, agrega exit codes.

set -u
cd "$(dirname "$0")/.."

fail=0

echo "=== VM unit tests ==="
if ./build/test_vm; then :; else fail=$((fail + $?)); fi
echo

echo "=== compiler golden tests ==="
if bash tests/test_compiler.sh; then :; else fail=$((fail + $?)); fi
echo

echo "=== compiler compile-time error tests ==="
if bash tests/test_compile_errors.sh; then :; else fail=$((fail + $?)); fi
echo

if [ $fail -eq 0 ]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "TESTS FAILED (aggregate exit = $fail)"
    exit 1
fi
