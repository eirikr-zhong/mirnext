#!/usr/bin/env python3
"""Regenerate offline SIMD fixtures from a checkout of the pinned Core 2.0 spec.

Usage: python3 tests/SIMD/GenerateCases.py /path/to/spec
This is a developer tool, never invoked by CMake or by product code.
"""
# Licensed under the MIT License. Generated fixtures retain upstream Apache-2.0 terms.
import collections
import math
from pathlib import Path
import re
import struct
import sys

REVISION = "fffc6e12fa454e475455a7b58d3b5dc343980c10"  # wg-2.0
ROOT = Path(__file__).resolve().parent


def instructions(source):
    aliases = {"ALLTRUE": "all_true", "ANYTRUE": "any_true", "EXTADDPAIRWISE": "extadd_pairwise",
               "EXTRACTLANE": "extract_lane", "REPLACELANE": "replace_lane", "Q15MULRSAT": "q15mulr_sat",
               "VCONST": "const", "VEXTEND": "extend", "VCONVERT": "convert", "VPROMOTE": "promote",
               "VDEMOTE": "demote", "VPMIN": "pmin", "VPMAX": "pmax"}
    result = []
    for line in (source / "document/core/binary/instructions.rst").read_text().splitlines():
        if r"\hex{FD}" not in line or r"\Rightarrow&" not in line:
            continue
        number = re.search(r"~~(\d+)\{:", line)
        name = re.search(r"\\(\w+)\.\\(\w+)(?:\\K\{([^}]+)\})?", line.split(r"\Rightarrow&")[1])
        assert number and name, line
        shape, operation, suffix = name.groups()
        operation = aliases.get(operation, operation[1:].lower() if operation.startswith("V") else operation.lower())
        name = shape.lower() + "." + operation + (suffix or "").replace("\\_", "_")
        result.append((int(number[1]), name))
    assert len(result) == 236 and len(set(n for _, n in result)) == 236
    return sorted(result)


def forms(text):
    text = re.sub(r"\(;.*?;\)", "", text, flags=re.S)
    text = re.sub(r";;[^\n]*", "", text)
    stack = []
    for token in re.findall(r'"(?:\\.|[^"\\])*"|[^\s()]+|[()]', text):
        if token == "(":
            stack.append([])
        elif token == ")":
            form = stack.pop()
            if stack:
                stack[-1].append(form)
            else:
                yield form
        else:
            stack[-1].append(token)


def constant(form, expected=False):
    if not isinstance(form, list):
        raise ValueError("not a constant")
    tag, *values = form
    if tag == "v128.const":
        shape, *values = values
        width = int(re.search(r"\d+", shape)[0])
        assert len(values) == 128 // width
    elif tag in ("i32.const", "i64.const", "f32.const", "f64.const"):
        shape = tag.split(".")[0]
        width = int(shape[1:])
    else:
        raise ValueError("not a literal constant")
    data = bytearray()
    nan_mask = 0
    for lane, literal in enumerate(values):
        literal = literal.replace("_", "").lower()
        if shape.startswith("i"):
            value = int(literal, 16 if "0x" in literal else 10) & ((1 << width) - 1)
        elif "nan" in literal:
            value = (0x7fc00000 if width == 32 else 0x7ff8000000000000)
            if ":0x" in literal:
                value = (0x7f800000 if width == 32 else 0x7ff0000000000000) | int(literal.split(":")[1], 16)
            if literal.startswith("-"):
                value |= 1 << (width - 1)
            if expected:
                nan_mask |= 1 << lane
        else:
            v = float.fromhex(literal) if "0x" in literal else float(literal)
            value = int.from_bytes(struct.pack("<f" if width == 32 else "<d", v), "little")
        data += value.to_bytes(width // 8, "little")
    return shape, data.hex(), nan_mask


def string_bytes(token):
    data = bytearray()
    literal = token[1:-1]
    n = 0
    while n < len(literal):
        if literal[n] == "\\":
            data.append(int(literal[n + 1:n + 3], 16))
            n += 3
        else:
            data.extend(literal[n].encode("utf-8"))
            n += 1
    return data


def main():
    source = Path(sys.argv[1])
    listing = instructions(source)
    known = {name for _, name in listing}
    (ROOT / "Instructions.def").write_text(
        "/* Core 2.0 SIMD128 instruction inventory; generated from wg-2.0.\n"
        " * Upstream W3C Software and Document License; see DOCUMENT-LICENSE. */\n" +
        "".join(f'MIRNEXT_STANDARD_SIMD (0x{number:02x}, "{name}")\n' for number, name in listing))
    counts = collections.Counter()
    lines = ["# Derived from WebAssembly/spec " + REVISION,
             "# Apache-2.0; see LICENSE. Literal assert_return cases only. Fields: instruction; inputs (shape:hex); expected shape:hex; NaN lane mask; source file"]
    for path in sorted((source / "test/core/simd").glob("*.wast")):
        aliases = {}
        memory = bytearray()
        for form in forms(path.read_text()):
            if form and form[0] == "module":
                aliases = {}
                memory = bytearray()
                for declaration in form[1:]:
                    if isinstance(declaration, list) and declaration and declaration[0] == "memory":
                        sizes = [int(v) for v in declaration[1:] if isinstance(v, str) and v.isdigit()]
                        if sizes: memory = bytearray(sizes[0] * 65536)
                    if isinstance(declaration, list) and declaration and declaration[0] == "data":
                        if len(declaration) >= 3 and isinstance(declaration[1], list) and declaration[1][0] == "i32.const":
                            offset = int(declaration[1][1], 0)
                            data = b"".join(string_bytes(v) for v in declaration[2:])
                            memory[offset:offset + len(data)] = data
                for function in form[1:]:
                    if not isinstance(function, list) or not function or function[0] != "func": continue
                    exports = [v[1].strip('"') for v in function if isinstance(v, list) and v and v[0] == "export"]
                    expression = function[-1]
                    if not isinstance(expression, list) or expression[0] not in known: continue
                    if not all(isinstance(v, list) and v and v[0] == "local.get" for v in expression[1:]): continue
                    for export in exports: aliases[export] = expression[0]
            if not form or form[0] != "assert_return" or len(form) != 3:
                continue
            invoke = form[1]
            if not isinstance(invoke, list) or invoke[0] != "invoke":
                continue
            name = aliases.get(invoke[1].strip('"'))
            if name not in known:
                continue
            try:
                inputs = [constant(v) for v in invoke[2:]]
                expected = constant(form[2], True)
            except (ValueError, AssertionError, OverflowError):
                continue
            if name.startswith("v128.load"):
                assert len(inputs) == 1 and inputs[0][0] == "i32"
                offset = int.from_bytes(bytes.fromhex(inputs[0][1]), "little")
                width = 8 if "x" in name.split(".")[1] else int(re.search(r"load(\d+)", name)[1]) // 8
                assert offset + width <= len(memory)
                inputs = [("memory", memory[offset:offset + width].hex(), 0)]
            lines.append(";".join([name, ",".join(f"{s}:{h}" for s, h, _ in inputs),
                                   f"{expected[0]}:{expected[1]}", str(expected[2]), path.name]))
            counts[name] += 1
    (ROOT / "Cases.tsv").write_text("\n".join(lines) + "\n")
    (ROOT / "LICENSE").write_bytes((source / "test/LICENSE").read_bytes())
    (ROOT / "DOCUMENT-LICENSE").write_bytes((source / "document/LICENSE").read_bytes())
    print(f"236 standard instructions; {sum(counts.values())} literal cases for {len(counts)} instructions")
    print("Special-immediate/memory instructions covered by the C++ adapter:", sorted(known - counts.keys()))


if __name__ == "__main__":
    main()
