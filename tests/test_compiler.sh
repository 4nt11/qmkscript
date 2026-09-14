#!/usr/bin/env bash
# tests/test_compiler.sh -- golden bytecode tests del compilador.
# Compila cada examples/*.qks y verifica que el bytecode hex (via xxd) es
# idéntico al esperado en tests/golden/*.hex.
#
# Un cambio al lexer/parser/emit_bytecode que altere el bytecode de un
# ejemplo dispara diff -> exit != 0. Si el cambio es intencional, regenerar
# el golden y explicar en el commit.

set -u   # exit on unset variable use. NO -e: queremos continuar y agregar fallos.

# Colores (respetan NO_COLOR)
if [ -z "${NO_COLOR:-}" ] && [ -t 1 ]; then
    R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; N=$'\033[0m'
else
    R=; G=; Y=; N=
fi

# cwd: raíz del proyecto (script llamado como tests/test_compiler.sh)
cd "$(dirname "$0")/.."

QMKSCRIPT=./build/qmkscript
if [ ! -x $QMKSCRIPT ]; then
    echo "${R}FAIL${N}: $QMKSCRIPT no existe. Corre 'make' primero."
    exit 1
fi

pass=0
fail=0
tests=$(ls tests/golden/*.hex | wc -l)

for golden in tests/golden/*.hex; do
    name=$(basename "$golden" .hex)
    src="examples/${name}.qks"
    if [ ! -f "$src" ]; then
        printf "${Y}SKIP${N}  %-24s (no source: %s)\n" "$name" "$src"
        continue
    fi

    actual=$($QMKSCRIPT --emit=bytecode "$src" 2>/dev/null | xxd)
    expected=$(cat "$golden")

    if [ "$actual" = "$expected" ]; then
        printf "${G} OK ${N}  %s\n" "$name"
        pass=$((pass+1))
    else
        printf "${R}FAIL${N}  %s\n" "$name"
        diff <(echo "$expected") <(echo "$actual") | sed 's/^/       /'
        fail=$((fail+1))
    fi
done

echo "---"
echo "compiler tests: $pass/$tests OK, $fail FAIL"
[ $fail -eq 0 ]
