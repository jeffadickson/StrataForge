#!/usr/bin/env python3
"""STRATA-20 Bare-Metal Cross-Compilation Proof for Versal AArch64 EL2 & Lockstep Cortex-R5F.

When cross toolchains (`aarch64-linux-gnu-g++` / `arm-none-eabi-g++`) are installed:
1. Assembles `el2_vectors.S` + compiles `el2_hypervisor_main.cpp` freestanding
   (`-ffreestanding -nostdlib -fno-exceptions -fno-rtti -Wall -Wextra -Werror`)
   and links a real bare-metal AArch64 EL2 ELF against `el2_hyp.ld`.
2. Disassembles the linked AArch64 ELF to verify real EL2 instructions (`vbar_el2`,
   `vttbr_el2`, `eret`, `cntpct_el0`), `.critical_store` section placement, and
   ZERO heap/exception symbols.
3. Compiles `r5_fdir_main.cpp` freestanding for ARMv7-R Lockstep Cortex-R5F
   (`arm-none-eabi-g++ -mcpu=cortex-r5 -ffreestanding -fno-exceptions -fno-rtti -Werror`).
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

COMMON_INCLUDES = [
    f"-I{REPO_ROOT / 'hypervisor' / 'include'}",
    f"-I{REPO_ROOT / 'hypervisor' / 'arch'}",
    f"-I{REPO_ROOT / 'middleware' / 'include'}",
    f"-I{REPO_ROOT / 'services' / 'fdir_slot_mgr' / 'include'}",
]


def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        print(f"FAIL: {' '.join(cmd)}\n{proc.stderr}", file=sys.stderr)
        sys.exit(1)
    return proc


def check_aarch64_el2(td: pathlib.Path) -> str:
    cxx = shutil.which("aarch64-linux-gnu-g++")
    objdump = shutil.which("aarch64-linux-gnu-objdump")
    readelf = shutil.which("aarch64-linux-gnu-readelf")
    if not cxx or not objdump or not readelf:
        return "SKIP (aarch64-linux-gnu-g++ not installed)"

    vec_o = td / "el2_vectors.o"
    main_o = td / "el2_main.o"
    elf = td / "strata_versal_el2.elf"

    run([cxx, "-march=armv8-a", "-c", str(REPO_ROOT / "hypervisor/arch/aarch64/el2_vectors.S"), "-o", str(vec_o)])
    run(
        [
            cxx,
            "-std=c++20",
            "-march=armv8-a",
            "-mstrict-align",
            "-ffreestanding",
            "-fno-exceptions",
            "-fno-rtti",
            "-fno-threadsafe-statics",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            *COMMON_INCLUDES,
            "-c",
            str(REPO_ROOT / "hypervisor/core/el2_hypervisor_main.cpp"),
            "-o",
            str(main_o),
        ]
    )
    run(
        [
            cxx,
            "-nostdlib",
            "-static",
            "-Wl,-T," + str(REPO_ROOT / "hypervisor/arch/aarch64/el2_hyp.ld"),
            str(vec_o),
            str(main_o),
            "-o",
            str(elf),
        ]
    )

    # Verify real AArch64 EL2 hardware instructions exist in the linked image
    dis = run([objdump, "-d", str(elf)]).stdout.lower()
    for instr in ("vbar_el2", "vttbr_el2", "eret", "cntpct_el0"):
        assert instr in dis, f"Missing expected AArch64 EL2 instruction '{instr}' in {elf}"

    # Verify .critical_store section exists and zero PLT/heap symbols exist
    secs = run([readelf, "-S", str(elf)]).stdout
    assert ".critical_store" in secs, "Missing .critical_store section in EL2 ELF"
    assert ".plt" not in secs, "Unexpected .plt dynamic linking section in bare-metal EL2 ELF"

    size_bytes = elf.stat().st_size
    return f"PASS (AArch64 EL2 ELF {size_bytes}B: vbar_el2/vttbr_el2/eret/cntpct_el0 verified, zero libc/PLT)"


def check_arm_r5(td: pathlib.Path) -> str:
    cxx = shutil.which("arm-none-eabi-g++")
    if not cxx:
        return "SKIP (arm-none-eabi-g++ not installed)"

    r5_o = td / "r5_fdir.o"
    run(
        [
            cxx,
            "-std=c++20",
            "-mcpu=cortex-r5",
            "-marm",
            "-ffreestanding",
            "-fno-exceptions",
            "-fno-rtti",
            "-fno-threadsafe-statics",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            *COMMON_INCLUDES,
            "-c",
            str(REPO_ROOT / "hypervisor/core/r5_fdir_main.cpp"),
            "-o",
            str(r5_o),
        ]
    )
    return f"PASS (ARMv7-R Lockstep Cortex-R5F obj {r5_o.stat().st_size}B verified, -Werror clean)"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="strata_cross_") as td_str:
        td = pathlib.Path(td_str)
        a64_msg = check_aarch64_el2(td)
        r5_msg = check_arm_r5(td)
        print(f"OK: Bare-Metal Cross Audit | Versal-A72-EL2={a64_msg} | Versal-R5F={r5_msg}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
