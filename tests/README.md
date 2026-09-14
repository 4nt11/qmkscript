# qmkscript tests

Red de seguridad. Cero frameworks: `<assert.h>` + shell + `diff`. Corre con:

```
make test
```

## Estructura

- `run.sh` — entry point. Corre los dos suites y agrega exit codes.
- `test_vm.c` — micro-tests unitarios de `vm.c`. Compilado a `build/test_vm`, linkea directo con la implementación de la VM. Ejerce:
  - Guards LangSec (stack over/underflow, var OOB, jmp OOB, div zero)
  - Opcodes de efecto (STR/TAP/DELAY/CHORD/REG_MODS/UNREG_MODS) via capture ops table
  - Aritmética/comparación/jumps indirectamente via callbacks observables
- `test_compiler.sh` — golden bytecode tests. Compila cada `.qks` de `examples/` y verifica que el bytecode hex es idéntico al esperado en `golden/`.
- `golden/*.hex` — bytecode esperado, formato `xxd` (canonical). Un solo diff dispara el test.

## Cuándo actualizar los golden

Si un cambio al compilador es INTENCIONAL y cambia el bytecode de un `.qks`,
regenerar el golden con:

```
./build/qmkscript --emit=bytecode examples/hola.qks | xxd > tests/golden/hola.hex
```

Y explicar el cambio en el commit message. Regenerar sin explicar = red de seguridad rota.

## Filosofía

Los tests son la única forma de saber que la fase B/C.1/… siguen vivas
después de tocar la VM o el compilador. Si un test rojo aparece por un
cambio "trivial", parar y entender por qué antes de tocar el golden.

Ver [[qmkscript-design-philosophy]] para el contexto ideológico (defensa
en profundidad + verificar temprano).
