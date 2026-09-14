#!/usr/bin/env python3
"""gen_layouts.py -- extrae las layout tables de QMK y genera un header C.

Mismo pattern que gen_keycodes.py: parseamos los sendstring_*.h de vial-qmk,
resolvemos los alias ES_*/DE_*/etc via los keymap_*.h respectivos, y
emitimos src/layouts_generated.h con:

  - qks_layout_entry_t (kc: u8, mods: u8, dead: u8) x 128 por layout
  - qks_layout_table_t con las N layouts registradas
  - LAYOUT_US, LAYOUT_LATAM, ... constantes visibles al lexer (via
    qks_lookup_layout, lexer intenta esto tras qks_lookup_keycode)

Filosofía: layouts como DATOS extraídos del standard (QMK), no cableados
a mano. Cuando QMK añade una layout más, agregamos su tupla acá y ya.

Usage: python3 tools/gen_layouts.py [/path/to/vial-qmk]
"""
import os, re, sys

DEFAULT_QMK = os.path.expanduser("~/vial-qmk")

# Registro de layouts a incluir. Nombre lógico -> (keymap_*.h, sendstring_*.h)
# El nombre lógico también termina como constante LAYOUT_<NAME>.
LAYOUTS = [
    # (nombre_lógico, keymap_alias_file, sendstring_LUT_file)
    # US NO se emite -- current_layout=0 usa send_string (QMK's default,
    # US-hardcoded). Solo layouts NO-US necesitan table lookup.
    ("LATAM", "keymap_spanish_latin_america.h",    "sendstring_spanish_latin_america.h"),
    ("ES",    "keymap_spanish.h",                  "sendstring_spanish.h"),
]

# Constantes de mods HID (bits del modifier byte, coincide con vm.h VMMOD_*)
MOD_LCTL = 0x01
MOD_LSFT = 0x02
MOD_LALT = 0x04
MOD_LGUI = 0x08
MOD_RCTL = 0x10
MOD_RSFT = 0x20
MOD_RALT = 0x40
MOD_RGUI = 0x80

# Extras Latin-1 (codepoints 0x80..0xFF) para layouts basados en español.
# Cada entry: (codepoint, [(alias_kc, mods), ...])  -- 1 o 2 steps.
# Layouts LATAM y ES comparten el mismo alias set ES_*, así que este template
# vale para ambos (los aliases resuelven a kc distintos en cada keymap).
EXTRAS_SPANISH = [
    (0x00A1, [("ES_IQUE", MOD_LSFT)]),                       # ¡
    (0x00BF, [("ES_IQUE", 0)]),                              # ¿
    (0x00F1, [("ES_NTIL", 0)]),                              # ñ
    (0x00D1, [("ES_NTIL", MOD_LSFT)]),                       # Ñ
    (0x00E1, [("ES_ACUT", 0), ("ES_A", 0)]),                 # á  (dead-acute + a)
    (0x00E9, [("ES_ACUT", 0), ("ES_E", 0)]),                 # é
    (0x00ED, [("ES_ACUT", 0), ("ES_I", 0)]),                 # í
    (0x00F3, [("ES_ACUT", 0), ("ES_O", 0)]),                 # ó
    (0x00FA, [("ES_ACUT", 0), ("ES_U", 0)]),                 # ú
    (0x00C1, [("ES_ACUT", 0), ("ES_A", MOD_LSFT)]),          # Á
    (0x00C9, [("ES_ACUT", 0), ("ES_E", MOD_LSFT)]),          # É
    (0x00CD, [("ES_ACUT", 0), ("ES_I", MOD_LSFT)]),          # Í
    (0x00D3, [("ES_ACUT", 0), ("ES_O", MOD_LSFT)]),          # Ó
    (0x00DA, [("ES_ACUT", 0), ("ES_U", MOD_LSFT)]),          # Ú
    (0x00FC, [("ES_ACUT", MOD_LSFT), ("ES_U", 0)]),          # ü  (dead-diaeresis + u)
    (0x00DC, [("ES_ACUT", MOD_LSFT), ("ES_U", MOD_LSFT)]),   # Ü
]

EXTRAS = {
    "LATAM": EXTRAS_SPANISH,
    "ES":    EXTRAS_SPANISH,
}

# ---------------------------------------------------------------
# Parsers
# ---------------------------------------------------------------
ALIAS_RE = re.compile(r'^\s*#define\s+([A-Z][A-Z0-9_]+)\s+([A-Z][A-Z0-9_]+)\s*(?://.*)?$')

def parse_aliases(path):
    """Lee keymap_*.h y devuelve dict {ALIAS: TARGET} (ambos identifiers)."""
    aliases = {}
    if not os.path.exists(path):
        return aliases
    with open(path) as f:
        for line in f:
            m = ALIAS_RE.match(line)
            if m:
                aliases[m.group(1)] = m.group(2)
    return aliases

def parse_qmk_keycodes(path):
    """Reusa el enum de keycodes.h: {NAME: int_value} con aliases resueltos."""
    lit_re = re.compile(r'^\s*([A-Z][A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)\s*,?\s*$')
    al_re  = re.compile(r'^\s*([A-Z][A-Z0-9_]+)\s*=\s*([A-Z][A-Z0-9_]+)\s*,?\s*$')
    literals, aliases = {}, {}
    with open(path) as f:
        for line in f:
            m = lit_re.match(line)
            if m:
                literals[m.group(1)] = int(m.group(2), 16)
                continue
            m = al_re.match(line)
            if m:
                aliases[m.group(1)] = m.group(2)
    resolved = dict(literals)
    for name, target in aliases.items():
        cur = target
        for _ in range(10):
            if cur in literals:
                resolved[name] = literals[cur]; break
            cur = aliases.get(cur)
            if cur is None: break
    return resolved

def _strip_c_comments(text):
    """Elimina // y /* */ para que el regex de arrays no se confunda con
    comentarios que contienen `{` o `}` (típicamente los headers de las
    filas del LUT: `// { | }`).
    """
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    return text

def parse_keycode_array(text, name, alias_map, keycode_map):
    """Extrae 128 identifiers del array const uint8_t <name>[128] = { ... };
    Resuelve alias (ES_A -> KC_A -> 0x04). XXXXXXX -> 0.
    """
    text = _strip_c_comments(text)
    m = re.search(r'const\s+uint8_t\s+' + re.escape(name) + r'\s*\[\s*128\s*\][^{]*\{([^}]*)\}',
                  text, re.DOTALL)
    if not m:
        return None
    body = m.group(1)
    tokens = [t.strip() for t in body.split(',') if t.strip()]
    if len(tokens) != 128:
        raise ValueError(f'{name}: expected 128 tokens, got {len(tokens)}')
    out = []
    for tok in tokens:
        if tok == 'XXXXXXX' or tok == '0' or tok == 'KC_NO':
            out.append(0)
            continue
        # Resolve alias chain: tok -> alias_map[tok] -> ... -> keycode
        cur = tok
        for _ in range(10):
            if cur in keycode_map:
                out.append(keycode_map[cur] & 0xFF)  # solo el byte bajo (HID keycode)
                break
            cur = alias_map.get(cur)
            if cur is None:
                out.append(0); break
        else:
            out.append(0)
    return out

def parse_bit_lut(text, name):
    """Extrae const uint8_t <name>[16] con 16 KCLUT_ENTRY(a,b,c,d,e,f,g,h).
    Devuelve lista de 128 bools (LSB primero por entry, orden natural)."""
    text = _strip_c_comments(text)
    m = re.search(r'const\s+uint8_t\s+' + re.escape(name) + r'\s*\[\s*16\s*\][^{]*\{([^}]*)\}',
                  text, re.DOTALL)
    if not m:
        return None
    body = m.group(1)
    entries = re.findall(r'KCLUT_ENTRY\s*\(([^)]+)\)', body)
    if len(entries) != 16:
        raise ValueError(f'{name}: expected 16 KCLUT_ENTRY, got {len(entries)}')
    bits = []
    for e in entries:
        vals = [v.strip() for v in e.split(',')]
        if len(vals) != 8:
            raise ValueError(f'{name}: KCLUT_ENTRY needs 8 args')
        for v in vals:
            bits.append(v not in ('0', '0u'))
    return bits

# ---------------------------------------------------------------
# Main
# ---------------------------------------------------------------
def _resolve_alias(name, aliases, qmk_keycodes, max_hops=10):
    seen = set()
    while name in aliases and name not in seen and max_hops:
        seen.add(name); name = aliases[name]; max_hops -= 1
    return qmk_keycodes.get(name)

def build_layout(qmk_root, name, keymap_h, sendstring_h, qmk_keycodes):
    keymap_path = os.path.join(qmk_root, 'quantum/keymap_extras', keymap_h)
    ss_path     = os.path.join(qmk_root, 'quantum/keymap_extras', sendstring_h)
    if not os.path.exists(ss_path):
        print(f'  WARNING: {name}: sendstring file missing ({ss_path}), skipping',
              file=sys.stderr)
        return None
    aliases = parse_aliases(keymap_path)
    with open(ss_path) as f:
        text = f.read()
    kc_lut     = parse_keycode_array(text, 'ascii_to_keycode_lut', aliases, qmk_keycodes)
    shift_lut  = parse_bit_lut(text, 'ascii_to_shift_lut')  or [False]*128
    altgr_lut  = parse_bit_lut(text, 'ascii_to_altgr_lut')  or [False]*128
    dead_lut   = parse_bit_lut(text, 'ascii_to_dead_lut')   or [False]*128
    if kc_lut is None:
        print(f'  WARNING: {name}: no keycode LUT found, skipping', file=sys.stderr)
        return None
    entries = []
    for i in range(128):
        mods = 0
        if shift_lut[i]:  mods |= MOD_LSFT
        if altgr_lut[i]:  mods |= MOD_RALT
        entries.append((kc_lut[i], mods, 1 if dead_lut[i] else 0))
    # Resolver EXTRAS Latin-1 para este layout (si aplica).
    extras = {}   # codepoint -> [(kc, mods), ...]
    for cp, steps in EXTRAS.get(name, []):
        resolved = []
        for alias, mods in steps:
            kc = _resolve_alias(alias, aliases, qmk_keycodes)
            if kc is None:
                print(f'  WARNING: {name}: alias {alias} no resuelve, saltando cp U+{cp:04X}',
                      file=sys.stderr)
                resolved = None
                break
            resolved.append((kc, mods))
        if resolved:
            extras[cp] = resolved
    return entries, extras

def emit_header(layouts, out_path):
    lines = [
        "// AUTO-GENERATED by tools/gen_layouts.py from vial-qmk/quantum/keymap_extras/",
        "// DO NOT EDIT MANUALLY. Regenerate: make regen-layouts",
        "#pragma once",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "#include <string.h>",
        "",
        "typedef struct {",
        "    uint8_t kc;    // HID keycode",
        "    uint8_t mods;  // HID modifier bitmap (LSFT|RALT combos)",
        "    uint8_t dead;  // 1 si es dead key -> firmware tapea SPACE tras",
        "} qks_layout_entry_t;",
        "",
        "// Extras Latin-1 (codepoints 0x80..0xFF). Indexed por (codepoint - 128).",
        "// n=0 slot vacío; n=1 single tap; n=2 dead-key + letter.",
        "typedef struct {",
        "    uint8_t n;",
        "    uint8_t kc1, mods1;",
        "    uint8_t kc2, mods2;",
        "} qks_layout_extra_t;",
        "",
    ]
    # Emit each layout table (ASCII + extras)
    for i, (name, entries, extras) in enumerate(layouts):
        lines.append(f"// Layout {i}: {name} (ASCII 0..127)")
        lines.append(f"static const qks_layout_entry_t qks_layout_{name.lower()}[128] = {{")
        for j, (kc, mods, dead) in enumerate(entries):
            comment = ''
            if 0x20 <= j <= 0x7E:
                ch = chr(j) if j != ord('"') and j != ord('\\') else f'\\x{j:02x}'
                comment = f'  // {ch!r}'
            lines.append(f"    {{0x{kc:02X}, 0x{mods:02X}, {dead}}},{comment}")
        lines.append("};\n")
        # Extras Latin-1 (0x80..0xFF)
        lines.append(f"// Layout {i}: {name} extras (Latin-1 0x80..0xFF)")
        lines.append(f"static const qks_layout_extra_t qks_layout_{name.lower()}_extras[128] = {{")
        for slot in range(128):
            cp = 0x80 + slot
            if cp in extras:
                steps = extras[cp]
                if len(steps) == 1:
                    kc1, m1 = steps[0]
                    lines.append(f"    [0x{slot:02X}] = {{1, 0x{kc1:02X}, 0x{m1:02X}, 0, 0}},  // U+{cp:04X} {chr(cp)!r}")
                elif len(steps) == 2:
                    kc1, m1 = steps[0]
                    kc2, m2 = steps[1]
                    lines.append(f"    [0x{slot:02X}] = {{2, 0x{kc1:02X}, 0x{m1:02X}, 0x{kc2:02X}, 0x{m2:02X}}},  // U+{cp:04X} {chr(cp)!r}")
        lines.append("};\n")
    # Registry
    lines.append("static const qks_layout_entry_t * const qks_layouts[] = {")
    for name, _, _ in layouts:
        lines.append(f"    qks_layout_{name.lower()},")
    lines.append("};")
    lines.append("static const qks_layout_extra_t * const qks_layouts_extras[] = {")
    for name, _, _ in layouts:
        lines.append(f"    qks_layout_{name.lower()}_extras,")
    lines.append("};")
    lines.append(f"static const size_t qks_n_layouts = {len(layouts)};")
    lines.append("")
    # LAYOUT_* constants + lookup.
    # Convención: LAYOUT_DEFAULT=0 (send_string US), LAYOUT_LATAM=1 (tables[0]), ...
    lines.append("// Nombre -> índice runtime. current_layout=0 usa send_string (US default),")
    lines.append("// current_layout>0 usa tables[current_layout-1].")
    lines.append("typedef struct { const char *name; uint8_t idx; } qks_layout_name_t;")
    lines.append("static const qks_layout_name_t qks_layout_names[] = {")
    lines.append('    {"LAYOUT_DEFAULT", 0},  // send_string (US-hardcoded en QMK)')
    for i, (name, _, _) in enumerate(layouts):
        lines.append(f'    {{"LAYOUT_{name}", {i+1}}},')
    lines.append("};")
    lines.append(f"static const size_t qks_n_layout_names = {len(layouts) + 1};  // +1 por LAYOUT_DEFAULT")
    lines.append("")
    lines.append("// Retorna [0..N-1], o -1 si no existe.")
    lines.append("static inline int32_t qks_lookup_layout(const char *name) {")
    lines.append("    for (size_t i = 0; i < qks_n_layout_names; i++) {")
    lines.append("        if (strcmp(qks_layout_names[i].name, name) == 0)")
    lines.append("            return (int32_t)qks_layout_names[i].idx;")
    lines.append("    }")
    lines.append("    return -1;")
    lines.append("}")
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    total_bytes = len(layouts) * (128 * 3 + 128 * 5)
    print(f"wrote {out_path}: {len(layouts)} layouts ({total_bytes} bytes ascii+extras)")

def main():
    qmk = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_QMK
    kc_path = os.path.join(qmk, 'quantum/keycodes.h')
    if not os.path.exists(kc_path):
        sys.exit(f'no encuentro {kc_path}')
    qmk_keycodes = parse_qmk_keycodes(kc_path)
    print(f'parsed {len(qmk_keycodes)} QMK keycodes')
    built = []
    for name, keymap_h, sendstring_h in LAYOUTS:
        r = build_layout(qmk, name, keymap_h, sendstring_h, qmk_keycodes)
        if r is not None:
            entries, extras = r
            built.append((name, entries, extras))
            print(f'  {name}: 128 entries + {len(extras)} extras')
    if not built:
        sys.exit('no se generó ninguna layout')
    emit_header(built, 'src/layouts_generated.h')

if __name__ == '__main__':
    main()
