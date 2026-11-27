#!/usr/bin/env python3
"""STRATA-20 Live Bare-Metal QEMU AArch64 Cortex-A72 EL2 Execution Test (STRATA-VER-008).

Compiles & links `qemu_el2_boot.S` + `qemu_el2_selftest.cpp` freestanding for AArch64,
boots the ELF on `qemu-system-aarch64 -machine virt,virtualization=on -cpu cortex-a72`,
and verifies real hardware-emulated EL2 privilege, Stage-2 MMU Data Abort containment,
EL1 HVC#0 hypercalls, NvFaultLog SECDED warm-reboot survival, and bare-metal SHA-256.
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

INCLUDES = [
    f"-I{REPO_ROOT / 'hypervisor' / 'include'}",
    f"-I{REPO_ROOT / 'hypervisor' / 'arch'}",
    f"-I{REPO_ROOT / 'middleware' / 'include'}",
]


def run(cmd: list[str], timeout: float = 20.0) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)
    if proc.returncode != 0:
        print(f"FAIL: {' '.join(cmd)}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}", file=sys.stderr)
        sys.exit(1)
    return proc


def main() -> int:
    cxx = shutil.which("aarch64-linux-gnu-g++")
    qemu = shutil.which("qemu-system-aarch64")
    if not cxx or not qemu:
        print("SKIP: aarch64-linux-gnu-g++ or qemu-system-aarch64 not installed")
        return 0

    with tempfile.TemporaryDirectory(prefix="strata_qemu_el2_") as td_str:
        td = pathlib.Path(td_str)
        boot_o = td / "qemu_el2_boot.o"
        main_o = td / "qemu_el2_selftest.o"
        elf = td / "strata_qemu_el2.elf"

        run([cxx, "-march=armv8-a", "-c", str(REPO_ROOT / "hypervisor/arch/aarch64/qemu_el2_boot.S"), "-o", str(boot_o)])
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
                *INCLUDES,
                "-c",
                str(REPO_ROOT / "hypervisor/core/qemu_el2_selftest.cpp"),
                "-o",
                str(main_o),
            ]
        )
        run(
            [
                cxx,
                "-nostdlib",
                "-static",
                "-Wl,-T," + str(REPO_ROOT / "hypervisor/arch/aarch64/qemu_el2_virt.ld"),
                str(boot_o),
                str(main_o),
                "-o",
                str(elf),
            ]
        )

        res = run(
            [
                qemu,
                "-machine",
                "virt,virtualization=on",
                "-cpu",
                "cortex-a72",
                "-m",
                "128M",
                "-nographic",
                "-semihosting-config",
                "enable=on,target=native",
                "-kernel",
                str(elf),
            ],
            timeout=15.0,
        )

        assert "CurrentEL=2" in res.stdout, res.stdout
        assert "FAR_EL2=0x0000000080000000" in res.stdout, res.stdout
        assert "PASS: QEMU AArch64 EL2 Execution verified" in res.stdout, res.stdout
        print(res.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
