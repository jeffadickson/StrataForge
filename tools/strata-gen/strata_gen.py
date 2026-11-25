#!/usr/bin/env python3
"""STRATA-20 single-source-of-truth manifest validator and codegen entry point.

Validates static isolation, schedulability, and memory-pool non-overlap on a
`mission.toml` file, then computes the canonical 64-bit MANIFEST_HASH shared
across C++20 ICD headers, hypervisor tables, SystemVerilog SCSR, and XTCE.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys
import tomllib


def compute_manifest_hash(raw_bytes: bytes) -> int:
    digest = hashlib.sha256(raw_bytes).digest()
    return int.from_bytes(digest[:8], byteorder="little", signed=False)


def validate_manifest(data: dict) -> list[str]:
    errors: list[str] = []

    mission = data.get("mission")
    if not isinstance(mission, dict):
        return ["Missing [mission] section"]

    minor_us = int(mission.get("minor_frame_us", 0))
    major_us = int(mission.get("major_frame_us", 0))
    if minor_us <= 0 or major_us <= 0 or (major_us % minor_us) != 0:
        errors.append(
            f"Invalid frame timing: major_frame_us ({major_us}) must be a positive multiple of minor_frame_us ({minor_us})"
        )

    partitions = data.get("partitions", [])
    part_by_id: dict[str, dict] = {}
    cpu_budgets: dict[str, int] = {}
    for p in partitions:
        pid = p.get("id")
        if not pid:
            errors.append("Partition entry missing 'id'")
            continue
        if pid in part_by_id:
            errors.append(f"Duplicate partition id: {pid}")
        part_by_id[pid] = p
        cpu = p.get("cpu", "UNKNOWN")
        cpu_budgets[cpu] = cpu_budgets.get(cpu, 0) + int(p.get("budget_us", 0))

    for cpu, total_us in cpu_budgets.items():
        if total_us > minor_us:
            errors.append(
                f"Schedulability violation on {cpu}: sum(budget_us)={total_us} exceeds minor_frame_us={minor_us}"
            )

    # Check data pool non-overlap and power-of-two slot counts
    pools = data.get("data_pools", [])
    ranges: list[tuple[int, int, str]] = []
    for pool in pools:
        name = pool.get("name", "<unnamed>")
        base = int(pool.get("phys_base", 0))
        count = int(pool.get("slot_count", 0))
        size = int(pool.get("slot_bytes", 0))
        if count <= 0 or (count & (count - 1)) != 0:
            errors.append(f"Pool '{name}' slot_count ({count}) must be a power of two")
        if size <= 0 or (size % 4096) != 0:
            errors.append(f"Pool '{name}' slot_bytes ({size}) must be 4 KB aligned")
        end = base + count * size
        for r_start, r_end, r_name in ranges:
            if base < r_end and r_start < end:
                errors.append(f"Physical DDR overlap between pools '{name}' and '{r_name}'")
        ranges.append((base, end, name))

    # Check NoC criticality isolation on hardware slots
    for slot in data.get("hw_slots", []):
        owner = slot.get("owner_partition")
        if owner not in part_by_id:
            errors.append(f"Slot '{slot.get('name')}' references unknown owner_partition '{owner}'")
            continue
        crit = part_by_id[owner].get("criticality")
        noc_grp = slot.get("noc_routing_group", "")
        if crit in ("CLASS_C", "CLASS_D") and noc_grp == "CRITICAL_EXCL":
            errors.append(
                f"Isolation violation: {crit} slot '{slot.get('name')}' cannot use CRITICAL_EXCL NoC group"
            )

    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="STRATA-20 manifest validator & code generator")
    parser.add_argument("manifest", type=pathlib.Path, help="Path to mission.toml")
    args = parser.parse_args(argv)

    raw = args.manifest.read_bytes()
    data = tomllib.loads(raw.decode("utf-8"))
    errors = validate_manifest(data)
    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        return 1

    manifest_hash = compute_manifest_hash(raw)
    name = data["mission"]["name"]
    silicon = data["mission"]["target_silicon"]
    print(
        f"OK: {name} ({silicon}) | partitions={len(data.get('partitions', []))} "
        f"| hw_slots={len(data.get('hw_slots', []))} "
        f"| pools={len(data.get('data_pools', []))} "
        f"| MANIFEST_HASH=0x{manifest_hash:016X}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
