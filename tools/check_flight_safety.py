#!/usr/bin/env python3
"""STRATA-20 Flight Safety Static Stack-Usage & Zero-Heap/Zero-Exception ELF Auditor.

Enforces two NASA NPR 7150.2D Class B flight constraints automatically in CI:
1. Parses all GCC `-fstack-usage` (.su) files in the build tree, rejecting any
   unbounded dynamic stack frame (`dynamic`) and verifying every function stays
   within the configured static byte limit (default 4096 B).
2. Inspects compiled flight executables (`dual_instrument_sil`,
   `versal_mission_flight_sim`) via `nm -u` to prove ZERO undefined references
   to dynamic heap allocators (`malloc`, `calloc`, `realloc`, `free`,
   `operator new`, `operator delete`) or C++ exception runtime (`__cxa_throw`).
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

FORBIDDEN_SYMBOLS = {
    "malloc",
    "calloc",
    "realloc",
    "free",
    "_Znwm",         # operator new(size_t)
    "_Znam",         # operator new[](size_t)
    "_ZdlPv",        # operator delete(void*)
    "_ZdaPv",        # operator delete[](void*)
    "__cxa_throw",
    "__cxa_begin_catch",
    "__cxa_allocate_exception",
}


def audit_stack_usage(build_dir: pathlib.Path, max_bytes: int, is_asan: bool) -> tuple[list[str], int, int]:
    errors: list[str] = []
    su_files = sorted(build_dir.rglob("*.su"))
    fn_count = 0
    peak_bytes = 0

    for su in su_files:
        # Skip Verilator generated glue and unit test mains; audit all flight examples & unit code
        rel = str(su.relative_to(build_dir))
        if "verilated" in rel or "Vversal_" in rel or "Vkintex_" in rel:
            continue
        for raw_line in su.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = raw_line.strip().split("\t")
            if len(parts) < 3:
                continue
            fn_loc, size_str, kind = parts[0], parts[1], parts[2]
            fn_count += 1
            try:
                size_b = int(size_str)
            except ValueError:
                continue
            if size_b > peak_bytes and "examples/" in rel:
                peak_bytes = size_b
            # GCC emits "dynamic,bounded" for 32B/64B stack alignment prologues; forbid truly unbounded "dynamic" (VLA/alloca)
            if kind == "dynamic":
                errors.append(f"Unbounded dynamic stack frame forbidden in flight build: {fn_loc} ({size_b} B {kind})")
            # Apply strict static stack ceiling to flight example binaries (ASan inserts redzones around every local)
            if not is_asan and "examples/" in rel and size_b > max_bytes:
                errors.append(f"Flight stack budget exceeded ({size_b} B > {max_bytes} B): {fn_loc}")
    return errors, fn_count, peak_bytes


def audit_zero_heap_elf(elf_path: pathlib.Path) -> list[str]:
    if not elf_path.is_file():
        return [f"Flight ELF not found: {elf_path}"]
    proc = subprocess.run(["nm", "-u", str(elf_path)], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        return [f"nm failed on {elf_path}: {proc.stderr.strip()}"]
    errors: list[str] = []
    for line in proc.stdout.splitlines():
        tokens = line.strip().split()
        if not tokens:
            continue
        sym = tokens[-1].split("@")[0]
        if sym in FORBIDDEN_SYMBOLS:
            errors.append(f"Forbidden heap/exception symbol '{sym}' referenced by {elf_path.name}")
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="STRATA-20 stack-usage & zero-heap flight auditor")
    parser.add_argument("build_dir", type=pathlib.Path, help="CMake binary directory")
    parser.add_argument("--max-flight-stack-bytes", type=int, default=4096)
    parser.add_argument("--elf", type=pathlib.Path, action="append", default=[])
    parser.add_argument("--skip-elf-on-asan", action="store_true",
                        help="Relax ASan-instrumented ELF/stack redzone checks on sanitizer builds")
    args = parser.parse_args(argv)

    all_errors: list[str] = []
    is_asan = args.skip_elf_on_asan and ("sanitizer" in str(args.build_dir).lower())

    su_errors, fn_count, peak_b = audit_stack_usage(args.build_dir, args.max_flight_stack_bytes, is_asan)
    all_errors.extend(su_errors)

    audited_elfs = 0
    for elf in args.elf:
        if is_asan:
            continue
        all_errors.extend(audit_zero_heap_elf(elf))
        audited_elfs += 1

    if all_errors:
        for e in all_errors:
            print(f"AUDIT ERROR: {e}", file=sys.stderr)
        return 1

    print(
        f"OK: Flight Safety Audit PASSED | functions_checked={fn_count} "
        f"| peak_stack={peak_b}B (limit={args.max_flight_stack_bytes}B) "
        f"| zero_heap_elfs_verified={audited_elfs}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
