# qmkscript -- v0: parser bison + lexer flex + AST + dos backends.
CC      = gcc
FLEX    = flex
BISON   = bison
CFLAGS  = -Wall -Wextra -Wswitch-enum -O2 -g -Isrc -Ibuild

BIN     = build/qmkscript
OBJS    = build/main.o build/ast.o build/emit_c.o build/emit_vial.o \
          build/emit_bytecode.o build/qks_errors.o \
          build/parse.tab.o build/lex.yy.o

# Runtime host: intérprete de mesa (paso 2 del plan bytecode).
RUN_BIN  = build/qks-run
RUN_OBJS = build/qks_run.o build/vm.o build/vm_host.o

# Sender host: empuja .bin al AN360 por raw-HID (paso 3 del plan).
PUSH_BIN     = build/qks-push
PUSH_CFLAGS  = $(shell pkg-config --cflags hidapi-hidraw)
PUSH_LDFLAGS = $(shell pkg-config --libs   hidapi-hidraw)

LEX_BIN = build/lex-test    # herramienta de debug, standalone

# Test binary: micro-tests de la VM. Linkea directo con vm.o (misma copia
# que qks-run usa), así el test corre exactamente el mismo código.
TEST_VM_BIN  = build/test_vm
TEST_VM_OBJS = build/test_vm.o build/vm.o

all: $(BIN) $(RUN_BIN) $(PUSH_BIN) $(LEX_BIN)

$(BIN): $(OBJS) | build
	$(CC) $(CFLAGS) -o $@ $^

$(RUN_BIN): $(RUN_OBJS) | build
	$(CC) $(CFLAGS) -o $@ $^

# qks-push es un solo .c, compilo directo (una TU, un binario).
$(PUSH_BIN): tools/qks_push.c | build
	$(CC) $(CFLAGS) $(PUSH_CFLAGS) -o $@ $< $(PUSH_LDFLAGS)

# bison -d genera build/parse.tab.c Y build/parse.tab.h
build/parse.tab.c build/parse.tab.h: src/parse.y | build
	$(BISON) -d -o build/parse.tab.c src/parse.y

# lex integrado depende del .h de bison (contiene los IDs de token)
build/lex.yy.c: src/lex.l build/parse.tab.h | build
	$(FLEX) -o $@ src/lex.l

# compilar fuentes de src/
build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

# compilar fuentes generadas en build/
build/parse.tab.o: build/parse.tab.c build/parse.tab.h | build
	$(CC) $(CFLAGS) -Wno-unused-function -c $< -o $@

build/lex.yy.o: build/lex.yy.c build/parse.tab.h | build
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-sign-compare -c $< -o $@

# --- herramienta debug (standalone, no depende de bison) ---
build/lex-debug.yy.c: src/lex-debug.l | build
	$(FLEX) -o $@ $<

$(LEX_BIN): build/lex-debug.yy.c | build
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-sign-compare -o $@ $<

build:
	mkdir -p build

run: $(BIN)
	./$(BIN) examples/hola.qks

run-c: $(BIN)
	./$(BIN) --emit=c examples/hola.qks

run-vial: $(BIN)
	./$(BIN) --emit=vial examples/hola.qks

run-bc: $(BIN)
	./$(BIN) --emit=bytecode examples/hola.qks | xxd

run-bcdump: $(BIN)
	./$(BIN) --emit=bcdump examples/hola.qks

# Compila hola.qks a bytecode y lo pasa por el runtime (validación pipeline).
run-vm: $(BIN) $(RUN_BIN)
	./$(BIN) --emit=bytecode examples/hola.qks | ./$(RUN_BIN) -

# Compila y empuja al AN360 por raw-HID. Watch en otra terminal:
#     qmk console
run-push: $(BIN) $(PUSH_BIN)
	./$(BIN) --emit=bytecode examples/hola.qks | ./$(PUSH_BIN) -

# Idem pero usando el modo --push nativo de qmkscript (equivale al pipe de arriba).
run-push-native: $(BIN) $(PUSH_BIN)
	./$(BIN) --push examples/hola.qks

# Watch mode: cada save de examples/*.qks o *.qks en cwd -> auto bcdump.
# Overridable: `make watch WATCH=examples/foo.qks`.
# Requiere entr: `dnf install entr` (o `apt install entr` en debianoides).
WATCH ?= examples/*.qks
watch: $(BIN)
	@command -v entr >/dev/null || { \
		echo "watch: falta entr en el PATH."; \
		echo "  Fedora: sudo dnf install entr"; \
		echo "  Debian: sudo apt install entr"; \
		exit 1; }
	@echo "watch: mirando $(WATCH) -- Ctrl-C para salir"
	@ls $(WATCH) | entr -c ./$(BIN) --emit=bcdump /_

# Watch + push automático: cada save empuja el bytecode al kb (con qmk console
# corriendo aparte para ver logs).
watch-push: $(BIN) $(PUSH_BIN)
	@command -v entr >/dev/null || { \
		echo "watch-push: falta entr (`dnf install entr`)."; exit 1; }
	@echo "watch-push: cada save de $(WATCH) -> qks-push al AN360"
	@ls $(WATCH) | entr -c ./$(BIN) --push /_

lex: $(LEX_BIN)
	./$(LEX_BIN) examples/hola.qks

# --- tests (C.1.1: red de seguridad) ---
# test_vm.c linkea con vm.o para ejercer opcodes end-to-end sin front-end.
build/test_vm.o: tests/test_vm.c src/vm.h src/vm_errors.h | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_VM_BIN): $(TEST_VM_OBJS) | build
	$(CC) $(CFLAGS) -o $@ $^

# Corre todo: unit tests VM + golden tests compiler.
test: $(BIN) $(TEST_VM_BIN)
	@bash tests/run.sh

# Regenera src/keycodes_generated.h desde vial-qmk. Correr cada vez que QMK
# upstream añade keycodes nuevos. Default: parsea ~/vial-qmk. Override con
# QMK=<path>: make regen-keycodes QMK=/otro/vial-qmk
QMK ?= $(HOME)/vial-qmk
regen-keycodes:
	python3 tools/gen_keycodes.py $(QMK)

# Regenera src/layouts_generated.h desde vial-qmk. Correr cuando QMK
# actualice los sendstring_*.h o cuando queramos añadir un layout nuevo.
regen-layouts:
	python3 tools/gen_layouts.py $(QMK)

clean:
	rm -rf build

.PHONY: all run run-c run-vial run-bc run-bcdump run-vm run-push run-push-native watch watch-push lex test regen-keycodes regen-layouts clean
