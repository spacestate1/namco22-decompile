#!/usr/bin/env python3
"""prepare_roms.py -- non-destructive macOS ROM import for Time Crisis (TC1) and Time Crisis II (TC2).

Reads the ORIGINAL ROM folder(s) supplied by the user (loose chip files and/or
MAME zips inside them), validates every chip the same way the upstream setup
scripts do (exact expected byte size + MAME's CRC32, name first with a CRC
fallback), and copies ONLY the chips the upstream tables list into the
git-ignored extracted/ directories. The original files are only ever read, and
extra files in the source folder are ignored.

Then it builds the derived program images exactly as upstream does:
  - timecris/timecris_main.bin (68EC020 program, interleaved from ts2verb.1..4)
  - tc2/boot.bin, prog.bin, sub.bin, io.bin (what tc2's build translates to C)

Usage:
  python3 prepare_roms.py --tc1 <folder> [--tc2 <folder>]
  python3 prepare_roms.py --check
"""
import importlib.util
import os
import subprocess
import sys
import zipfile
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def load_upstream(path, modname):
    """Import an upstream setup script WITHOUT running script-level code:
    tc2/tools/setup_roms.py has no `__main__` guard, so exec_module would run
    its whole import flow (which tried to consume our --tc2 flag). Instead,
    import only if it is guarded; otherwise evaluate just its table assignments
    (CHIPS / EXTRA) via AST."""
    src = open(path, "r").read()
    import ast
    tree = ast.parse(src, path)
    m = importlib.util.module_from_spec(importlib.util.spec_from_loader(modname, loader=None))
    m.__file__ = path
    m.__dict__["__name__"] = modname
    import zlib as _zlib
    for mod in ("os", "sys", "zlib", "hashlib", "importlib"):
        m.__dict__[mod] = __import__(mod)
    has_guard = any(isinstance(n, ast.If) and isinstance(n.test, ast.Compare)
                    and getattr(getattr(n.test, "left", None), "id", None) == "__name__" for n in tree.body)
    if has_guard:
        spec = importlib.util.spec_from_file_location(modname, path)
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        return m
    for node in tree.body:
        if isinstance(node, (ast.Assign, ast.AnnAssign, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            code = ast.Module(body=[node], type_ignores=[])
            exec(compile(code, path, "exec"), m.__dict__)
    return m


def collect_files(folder):
    """basename(lower) -> bytes for top-level files plus zip members (read-only)."""
    if not os.path.isdir(folder):
        sys.exit(f"not a folder: {folder}")
    found = {}

    def take(name, data):
        found.setdefault(os.path.basename(name).lower(), data)

    for e in sorted(os.listdir(folder)):
        p = os.path.join(folder, e)
        if not os.path.isfile(p):
            continue
        if e.lower().endswith(".zip"):
            try:
                with zipfile.ZipFile(p) as z:
                    for i in z.infolist():
                        if not i.is_dir():
                            take(i.filename, z.read(i))
            except (zipfile.BadZipFile, OSError) as err:
                print(f"  {e}: not a usable zip ({err}); skipped")
        else:
            with open(p, "rb") as f:
                take(e, f.read())
    return found


def _load_tc1():
    return load_upstream(os.path.join(ROOT, "timecris", "tools", "setup_roms.py"), "timecris_setup_roms")


def _load_tc2():
    return load_upstream(os.path.join(ROOT, "tc2", "tools", "setup_roms.py"), "tc2_setup_roms")


def _tc2_complete(TC2):
    return all(os.path.isfile(os.path.join(TC2.DEST, n)) and os.path.getsize(os.path.join(TC2.DEST, n)) == s
               for n, (s, _) in TC2.CHIPS.items())


def import_tc1(folder):
    """Validate exactly like timecris/tools/setup_roms.py: name first, CRC fallback, matching size."""
    TC1 = _load_tc1()
    found = collect_files(folder)
    os.makedirs(TC1.DEST, exist_ok=True)
    by_crc = {}
    for v in found.values():
        by_crc.setdefault(zlib.crc32(v) & 0xFFFFFFFF, v)
    problems, data = [], {}
    for region, _kind, name, off, size, crc in TC1.CHIPS:
        d = found.get(name.lower())
        if d is None or zlib.crc32(d) & 0xFFFFFFFF != crc:
            d = by_crc.get(crc)
        if d is None:
            problems.append(f"missing (by name or CRC {crc:08x}): {name}")
        elif len(d) != size:
            problems.append(f"wrong size: {name} ({len(d)} bytes, expected {size})")
        else:
            data[name] = d
    if problems:
        sys.exit("TC1 chip set cannot be used:\n" + "\n".join(problems))
    for name, d in data.items():
        with open(os.path.join(TC1.DEST, name), "wb") as f:
            f.write(d)
    prg = bytearray(0x400000)
    for region, _kind, name, off, size, crc in TC1.CHIPS:
        if region == "maincpu":
            prg[off::4] = data[name]
    with open(os.path.join(TC1.ROOT, "timecris_main.bin"), "wb") as f:
        f.write(prg)
    print(f"TC1: {len(data)} chips -> {TC1.DEST}; timecris_main.bin built "
          f"(reset SSP {int.from_bytes(prg[0:4], 'big'):#010x} PC {int.from_bytes(prg[4:8], 'big'):#010x})")


def import_tc2(folder, found=None):
    TC2 = _load_tc2()
    if found is None:
        found = collect_files(folder)
    os.makedirs(TC2.DEST, exist_ok=True)
    missing = []
    for name, (size, crc) in TC2.CHIPS.items():
        d = found.get(name.lower())
        if d is None or len(d) != size or zlib.crc32(d) & 0xFFFFFFFF != crc:
            missing.append(name)
    if missing:
        sys.exit("TC2 chip set cannot be used: " + ", ".join(missing))
    for name in TC2.CHIPS:
        with open(os.path.join(folder, name), "rb") as f:
            data = f.read()
        with open(os.path.join(TC2.DEST, name), "wb") as f:
            f.write(data)
    print(f"TC2: {len(TC2.CHIPS)} chips -> {TC2.DEST} (each validated by size+CRC32)")
    # the program images the TC2 build translates (boot.bin, prog.bin, sub.bin, io.bin), made beside extracted/ by tc2's own tool
    tc2_dir = os.path.dirname(os.path.abspath(TC2.DEST))
    subprocess.run([sys.executable, os.path.join(tc2_dir, "tools", "make_prog.py"), TC2.DEST], cwd=tc2_dir, check=True)


def main():
    args = sys.argv[1:]
    if args == ["--check"]:
        tc1 = _load_tc1().have_complete()
        tc2 = _tc2_complete(_load_tc2())
        print(f"TC1 extracted/ complete: {tc1}")
        print(f"TC2 extracted/ complete: {tc2}")
        return 0 if (tc1 and tc2) else 1
    tc1_dir = tc2_dir = None
    i = 0
    while i < len(args):
        if args[i] == "--tc1" and i + 1 < len(args):
            tc1_dir = args[i + 1]
            i += 2
        elif args[i] == "--tc2" and i + 1 < len(args):
            tc2_dir = args[i + 1]
            i += 2
        else:
            sys.exit("usage: prepare_roms.py [--tc1 <folder>] [--tc2 <folder>] | --check")
    if not (tc1_dir or tc2_dir):
        sys.exit("nothing to do: pass --tc1 and/or --tc2 folders")
    if tc1_dir:
        import_tc1(tc1_dir)
    if tc2_dir:
        import_tc2(tc2_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
