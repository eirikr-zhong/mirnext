#!/usr/bin/env python3
"""Exercise format commands, runtime modes, host imports and diagnostics."""
import argparse
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for name in ("m2b", "b2m", "b2cpp", "run", "host", "work"):
    parser.add_argument("--" + name, required=True)
args = parser.parse_args()
work = Path(args.work)
work.mkdir(parents=True, exist_ok=True)


def invoke(command, data=None, environment=None):
    env = os.environ.copy()
    env.update(environment or {})
    if env.get("MIRNEXT_MODE") is None:
        env.pop("MIRNEXT_MODE", None)
    return subprocess.run([str(item) for item in command], input=data,
                          capture_output=True, env=env)


def expect(result, status=0, output=None, diagnostic=None):
    if (result.returncode != status or (output is not None and result.stdout.replace(b"\r\n", b"\n") != output)
            or (diagnostic is not None and diagnostic.encode() not in result.stderr)):
        raise RuntimeError(f"Unexpected command result: {result}")
    return result


def reject(result, diagnostic=None):
    if result.returncode == 0 or result.stdout or not result.stderr or (diagnostic and diagnostic.encode() not in result.stderr):
        raise RuntimeError(f"Missing atomic command diagnostic: {result}")


def convert(tool, source, destination):
    result = expect(invoke([tool], Path(source).read_bytes()))
    Path(destination).write_bytes(result.stdout)


def check_array(path):
    result = expect(invoke([args.b2cpp], path.read_bytes()))
    text = result.stdout.decode()
    encoded = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", text))
    if encoded != path.read_bytes() or "inline constexpr std::uint8_t mir_code" not in text:
        raise RuntimeError("Embedded bytes differ from the canonical binary")
    return len(result.stdout)

main = 'm:module\np:proto i32,p:message\nimport puts\nmain:func i64,i32:argc,p:argv,p:env\nlocal i64:r,p:s\nmov s,p:8(argv)\ncall p,puts,r,s\nmov s,p:(env)\nbfs invalid,s\nret argc\ninvalid:ret 99\nendfunc\nendmodule\n'
foreign = 'm:module\np:proto i64,i64:a,i64:b\nimport mirnext_cli_add\nmain:func i64\nlocal i64:r\ncall p,mirnext_cli_add,r,40,2\nret r\nendfunc\nendmodule\n'
(work / "main.mir").write_text(main, encoding="utf-8")
convert(args.m2b, work / "main.mir", work / "main.bmir")
convert(args.b2m, work / "main.bmir", work / "roundtrip.mir")
convert(args.m2b, work / "roundtrip.mir", work / "roundtrip.bmir")
if (work / "main.bmir").read_bytes() != (work / "roundtrip.bmir").read_bytes():
    raise RuntimeError("Command roundtrip is not canonical")
check_array(work / "main.bmir")
large = "large:module\nf:func i64\nlocal i64:r\n" + "".join(f"mov r,{n}\n" for n in range(1, 3001)) + "ret r\nendfunc\nendmodule\n"
(work / "large.mir").write_text(large, encoding="utf-8")
convert(args.m2b, work / "large.mir", work / "large.bmir")
if check_array(work / "large.bmir") < 16384:
    raise RuntimeError("Large embedded fixture did not exercise output buffer boundaries")
source = Path(__file__).resolve().parents[1]
for n in range(1, 17):
    original = work / f"test{n}"
    convert(args.m2b, source / f"Fixtures/Original/test{n}.mir", original.with_suffix(".bmir"))
    convert(args.b2m, original.with_suffix(".bmir"), original.with_suffix(".mir"))
    binary = work / f"test{n}.roundtrip.bmir"
    text = work / f"test{n}.roundtrip.mir"
    convert(args.m2b, original.with_suffix(".mir"), binary)
    convert(args.b2m, binary, text)
    if text.read_bytes() != original.with_suffix(".mir").read_bytes():
        raise RuntimeError(f"Corpus canonical text changed test{n}")
    canonical = work / f"test{n}.canonical.bmir"
    convert(args.m2b, text, canonical)
    if binary.read_bytes() != canonical.read_bytes():
        raise RuntimeError(f"Corpus canonical binary changed test{n}")
for mode in ("interp", "gen", "lazy", "bbv"):
    expect(invoke([args.run, work / "main.bmir", "program", "argument payload"],
                  environment={"MIRNEXT_MODE": mode, "MIRNEXT_LIBS": ""}), 2, b"argument payload\n")
(work / "foreign.mir").write_text(foreign, encoding="utf-8")
convert(args.m2b, work / "foreign.mir", work / "foreign.bmir")
for mode in ("interp", "gen", "lazy", "bbv"):
    expect(invoke([args.run, work / "foreign.bmir", "program"],
                  environment={"MIRNEXT_MODE": mode, "MIRNEXT_LIBS": args.host}), 42)
host = Path(args.host)
expect(invoke([args.run, work / "foreign.bmir", "program"],
              environment={"MIRNEXT_MODE": "lazy", "MIRNEXT_LIB_DIRS": str(host.parent),
                           "MIRNEXT_LIBS": re.sub(r"^lib", "", host.stem)}), 42)
expect(invoke([args.run, work / "foreign.bmir", "program"],
              environment={"MIRNEXT_MODE": None, "MIRNEXT_LIBS": args.host}), 42)
invalid = b"not a MIR program"
(work / "invalid").write_bytes(invalid)
for tool in (args.m2b, args.b2m, args.b2cpp):
    reject(invoke([tool], invalid))
for path in (work / "invalid", work / "not-present.bmir"):
    reject(invoke([args.run, path, "program"]))
reject(invoke([args.run, work / "main.bmir", "program"], environment={"MIRNEXT_MODE": "invalid"}), "unknown MIRNEXT_MODE")
reject(invoke([args.run, work / "foreign.bmir", "program"], environment={"MIRNEXT_LIBS": str(work / "missing-library")}), "cannot load MIR library")
rejected = [('no-main', 'm:module\nf:func i64\nret 0\nendfunc\nendmodule\n', 'without main'), ('main-signature', 'm:module\nmain:func d\nret 0.0\nendfunc\nendmodule\n', 'main requires'), ('main-argument', 'm:module\nmain:func i64,p:a,p:b\nret 0\nendfunc\nendmodule\n', 'main requires'), ('duplicate-main', 'm:module\nmain:func i64\nret 0\nendfunc\nendmodule\nn:module\nmain:func i64\nret 0\nendfunc\nendmodule\n', 'multiple main'), ('unresolved', 'm:module\np:proto i64\nimport mirnext_missing_symbol_2381\nmain:func i64\nlocal i64:r\ncall p,mirnext_missing_symbol_2381,r\nret r\nendfunc\nendmodule\n', 'cannot resolve host symbol'), ('untyped', 'm:module\nimport puts\nmain:func i64\nlocal p:r\nmov r,puts\nret r\nendfunc\nendmodule\n', 'requires a typed declaration'), ('conflicting-prototypes', 'm:module\np:proto i64,i64:x\nq:proto i64,d:x\nimport puts\nmain:func i64\nlocal i64:r\ncall p,puts,r,0\ncall q,puts,r,0.0\nret r\nendfunc\nendmodule\n', 'inconsistent call prototypes')]
for name, text, diagnostic in rejected:
    path = work / (name + ".mir")
    path.write_text(text, encoding="utf-8")
    binary = path.with_suffix(".bmir")
    convert(args.m2b, path, binary)
    reject(invoke([args.run, binary, "program"], environment={"MIRNEXT_MODE": "lazy", "MIRNEXT_LIBS": ""}), diagnostic)
for tool in (args.m2b, args.b2m, args.b2cpp, args.run):
    reject(invoke([tool, "unexpected"]), "usage:")
if Path("/dev/full").exists():
    with open("/dev/full", "wb") as output:
        result = subprocess.run([args.m2b], input=main.encode(), stdout=output, stderr=subprocess.PIPE)
    if result.returncode == 0 or b"write failed" not in result.stderr:
        raise RuntimeError(f"Output failure was not diagnosed: {result}")
print("Formats, embedded bytes, four engines, argv/env and shared symbols passed")
