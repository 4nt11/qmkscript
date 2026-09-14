#!/usr/bin/env bash
# tests/test_compile_errors.sh -- verifica que inputs inválidos son rechazados
# a compile-time con exit != 0 y mensaje que contiene un substring esperado.
#
# Cubre los guards del compilador -- complementa test_compiler.sh (golden
# positivos) con casos negativos.

set -u
cd "$(dirname "$0")/.."

if [ -z "${NO_COLOR:-}" ] && [ -t 1 ]; then
    R=$'\033[31m'; G=$'\033[32m'; N=$'\033[0m'
else
    R=; G=; N=
fi

pass=0
fail=0

# expect_fail <name> <input> <expected_stderr_substring>
expect_fail() {
    local name=$1 input=$2 needle=$3
    local out
    out=$(printf '%s\n' "$input" | ./build/qmkscript --emit=bytecode 2>&1 >/dev/null)
    local rc=$?
    if [ $rc -eq 0 ]; then
        printf "${R}FAIL${N}  %s -- se esperaba exit != 0 pero compiló OK\n" "$name"
        fail=$((fail+1))
        return
    fi
    if [[ "$out" == *"$needle"* ]]; then
        printf "${G} OK ${N}  %s (rc=%d, matches: %s)\n" "$name" $rc "$needle"
        pass=$((pass+1))
    else
        printf "${R}FAIL${N}  %s -- rc=%d pero stderr no contiene '%s':\n" "$name" $rc "$needle"
        echo "         $out"
        fail=$((fail+1))
    fi
}

echo "compile-time error tests:"
# Grepean el CÓDIGO tipado [E_XXX] en vez del texto libre. Más estable:
# si mañana cambia la wording del mensaje, el test sigue verde mientras
# el enum no cambie. Machine-readable.

expect_fail "var redeclarada"      'var x = 1
var x = 2'                          "[E_VAR_REDECLARED]"

# Genera 33 vars distintas -> excede VM_VARS_SIZE=32.
overflow_input=$(python3 -c "print('\n'.join(f'var v{i} = {i}' for i in range(33)))")
expect_fail "overflow de vars"      "$overflow_input"                                  "[E_VAR_LIMIT_EXCEEDED]"

expect_fail "tap con tecla desconocida" 'tap zzz'                                    "[E_KEY_UNKNOWN]"

expect_fail "delay fuera de rango"      'delay 99999'                                "[E_DELAY_OOR]"

expect_fail "var no declarada"          'var y = x'                                  "[E_VAR_UNDECLARED]"

# C.2.d: nonassoc en comparaciones. a<b<c es error de parseo (contra el
# bug clásico de C). El mensaje es genérico ("syntax error") porque
# bison no tiene contexto sobre por qué -- lo grepeamos igual.
expect_fail "nonassoc a<b<c" 'var a=1
var b=2
var c=3
var oops = a < b < c'                                                                "parse error"

# C.2.e: reasign de var no declarada = error compile-time (mismo enum
# que VAR_REF, distinto contexto -- el mensaje aclara "usa 'var' primero").
expect_fail "reasign sin declarar"      'x = 5'                                      "[E_VAR_UNDECLARED]"

# C.3.g: duplicate case detection.
expect_fail "switch case duplicado" 'var x = 1
switch x { case 5: type "a" case 5: type "b" }'                                       "[E_DUPLICATE_CASE]"

# C.4.a: bind() errors.
expect_fail "bind duplicado" 'bind(KC_A)
bind(KC_B)
type "x"'                                                                             "[E_BIND_DUPLICATE]"
expect_fail "keycode desconocido" 'bind(KC_NOPE_XYZ)
type "x"'                                                                             "[E_KEYCODE_UNKNOWN]"
expect_fail "bind chord multi-elem" 'bind(chord [gui, "hola"])
type "x"'                                                                             "[E_BIND_INVALID]"

echo "---"
echo "compile-time error tests: $pass OK, $fail FAIL"
[ $fail -eq 0 ]
