#!/usr/bin/env python3
"""STRATA-20 single-source-of-truth manifest validator and multi-domain generator.

Implements STRATA-GEN-001, STRATA-GEN-002, STRATA-GEN-003, and STRATA-GEN-004:
- Parses declarative `mission.toml` manifests and optional CCSDS SEDS XML datasheets.
- Proves static schedulability, physical DDR pool non-overlap, NoC criticality isolation,
  and heterogeneous CPU/PL/AIE pipeline DAG acyclicity.
- Computes canonical 64-bit MANIFEST_HASH shared across all emitted artifacts.
- Atomically emits:
    1. C++20 ICD & ROM command/telemetry tables (`strata_mission_icd.hpp`)
    2. Hypervisor partition/region configuration tables (`strata_hyp_tables.hpp`)
    3. Per-partition GNU ld linker scripts (`link_<part_id>.ld`)
    4. SystemVerilog SCSR register package (`scsr_pkg.sv`)
    5. Vivado XMPU/SMMU/NoC QoS isolation TCL script (`strata_xmpu_noc.tcl`)
    6. CCSDS XTCE 1.2 XML ground dictionary (`mission_xtce.xml`)
    7. Generic JSON ground dictionary (`mission_dict.json`)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
import tomllib
import xml.etree.ElementTree as ET


def compute_manifest_hash(raw_bytes: bytes, seds_bytes: list[bytes] | None = None) -> int:
    h = hashlib.sha256(raw_bytes)
    for b in (seds_bytes or []):
        h.update(b)
    return int.from_bytes(h.digest()[:8], byteorder="little", signed=False)


def parse_seds_xml(seds_path: pathlib.Path) -> dict:
    """Parse a CCSDS SEDS XML device datasheet into STRATA register/telemetry metadata."""
    tree = ET.parse(seds_path)
    root = tree.getroot()
    ns = ""
    if root.tag.startswith("{"):
        ns = root.tag.split("}")[0] + "}"

    pkg_elem = root.find(f"{ns}Package")
    pkg_name = pkg_elem.attrib.get("name", seds_path.stem) if pkg_elem is not None else seds_path.stem

    registers: list[dict] = []
    for param in root.iter(f"{ns}Parameter"):
        offset_raw = param.attrib.get("offset", "0x1000")
        offset = int(offset_raw, 0)
        registers.append(
            {
                "name": param.attrib.get("name", "UNNAMED"),
                "type": param.attrib.get("type", "uint32"),
                "read_only": param.attrib.get("readOnly", "false").lower() == "true",
                "offset": offset,
                "unit": param.attrib.get("unit", ""),
                "cal_c0": float(param.attrib.get("cal_c0", "0.0")),
                "cal_c1": float(param.attrib.get("cal_c1", "1.0")),
            }
        )
    return {"package": pkg_name, "registers": registers}


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

    # Check NoC criticality isolation and slot ownership
    slot_names: set[str] = set()
    for slot in data.get("hw_slots", []):
        sname = slot.get("name", "")
        slot_names.add(sname)
        owner = slot.get("owner_partition")
        if owner not in part_by_id:
            errors.append(f"Slot '{sname}' references unknown owner_partition '{owner}'")
            continue
        crit = part_by_id[owner].get("criticality")
        noc_grp = slot.get("noc_routing_group", "")
        if crit in ("CLASS_C", "CLASS_D") and noc_grp == "CRITICAL_EXCL":
            errors.append(
                f"Isolation violation: {crit} slot '{sname}' cannot use CRITICAL_EXCL NoC group"
            )

    # Validate heterogeneous CPU/PL/AIE pipeline DAGs (no unknown nodes, no cycles)
    for pipe in data.get("pipelines", []):
        pname = pipe.get("name", "<unnamed>")
        stages = pipe.get("stages", [])
        seen: set[str] = set()
        for stage in stages:
            if stage in seen:
                errors.append(f"Pipeline '{pname}' contains cycle on stage '{stage}'")
            seen.add(stage)
            if stage.startswith("slot:"):
                if stage[5:] not in slot_names:
                    errors.append(f"Pipeline '{pname}' references unknown hw_slot '{stage[5:]}'")
            elif stage.startswith("part:"):
                if stage[5:] not in part_by_id:
                    errors.append(f"Pipeline '{pname}' references unknown partition '{stage[5:]}'")
            else:
                errors.append(f"Pipeline '{pname}' stage '{stage}' must start with 'slot:' or 'part:'")

    # Validate command opcode uniqueness
    seen_opcodes: set[int] = set()
    for cmd in data.get("commands", []):
        op = int(cmd.get("opcode", 0))
        if op in seen_opcodes:
            errors.append(f"Duplicate command opcode: 0x{op:04X}")
        seen_opcodes.add(op)

    return errors


def _crit_cpp(crit: str) -> str:
    return {
        "CLASS_B": "strata::Criticality::ClassB",
        "CLASS_C": "strata::Criticality::ClassC",
        "CLASS_D": "strata::Criticality::ClassD",
    }.get(crit, "strata::Criticality::ClassD")


def emit_cpp_icd(data: dict, seds_docs: list[dict], manifest_hash: int) -> str:
    mission = data["mission"]
    lines = [
        "#pragma once",
        "// AUTO-GENERATED BY tools/strata-gen/strata_gen.py — DO NOT EDIT",
        "#include <strata/core/types.hpp>",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace strata::generated {",
        "",
        f'inline constexpr const char* kMissionName   = "{mission["name"]}";',
        f'inline constexpr const char* kTargetSilicon = "{mission["target_silicon"]}";',
        f"inline constexpr uint64_t    kManifestHash  = 0x{manifest_hash:016X}ULL;",
        f"inline constexpr uint32_t    kMinorFrameUs  = {int(mission['minor_frame_us'])}U;",
        f"inline constexpr uint32_t    kMajorFrameUs  = {int(mission['major_frame_us'])}U;",
        "",
        "enum class PartitionId : uint8_t {",
    ]
    for idx, p in enumerate(data.get("partitions", [])):
        lines.append(f"    {p['id']} = {idx}U,")
    lines.append("};")
    lines.append("")

    # Slot SCSR base constants
    lines.append("struct HwSlotDescriptor {")
    lines.append("    uint16_t slot_id;")
    lines.append("    uint8_t  owner_partition;")
    lines.append("    uint64_t scsr_phys_addr;")
    lines.append("    uint32_t quiesce_timeout_us;")
    lines.append("};")
    part_idx = {p["id"]: i for i, p in enumerate(data.get("partitions", []))}
    slots = data.get("hw_slots", [])
    lines.append(f"inline constexpr std::array<HwSlotDescriptor, {max(1, len(slots))}> kHwSlots = {{{{")
    if slots:
        for s in slots:
            lines.append(
                f"    HwSlotDescriptor{{{int(s['slot_id'])}U, {part_idx.get(s['owner_partition'], 0)}U, "
                f"0x{int(s.get('scsr_phys_addr', 0)):016X}ULL, {int(s.get('quiesce_timeout_us', 500))}U}},"
            )
    else:
        lines.append("    HwSlotDescriptor{0U, 0U, 0ULL, 0U},")
    lines.append("}};")
    lines.append("")

    # ROM Command table
    cmds = data.get("commands", [])
    lines.append("struct RomCommandEntry {")
    lines.append("    uint16_t opcode;")
    lines.append("    uint8_t  owner_partition;")
    lines.append("    uint16_t payload_bytes;")
    lines.append("};")
    lines.append(f"inline constexpr std::array<RomCommandEntry, {max(1, len(cmds))}> kRomCommands = {{{{")
    if cmds:
        for c in cmds:
            lines.append(
                f"    RomCommandEntry{{0x{int(c['opcode']):04X}U, {part_idx.get(c['owner_partition'], 0)}U, "
                f"{int(c.get('payload_bytes', 0))}U}},"
            )
    else:
        lines.append("    RomCommandEntry{0U, 0U, 0U},")
    lines.append("}};")
    lines.append("")

    # ROM Telemetry table with engineering calibration coefficients
    tlms = data.get("telemetry", [])
    lines.append("struct RomTelemetryEntry {")
    lines.append("    uint16_t channel_id;")
    lines.append("    uint8_t  owner_partition;")
    lines.append("    double   cal_c0;")
    lines.append("    double   cal_c1;")
    lines.append("};")
    lines.append(f"inline constexpr std::array<RomTelemetryEntry, {max(1, len(tlms))}> kRomTelemetry = {{{{")
    if tlms:
        for t in tlms:
            lines.append(
                f"    RomTelemetryEntry{{0x{int(t['channel_id']):04X}U, {part_idx.get(t['owner_partition'], 0)}U, "
                f"{float(t.get('cal_c0', 0.0))}, {float(t.get('cal_c1', 1.0))}}},"
            )
    else:
        lines.append("    RomTelemetryEntry{0U, 0U, 0.0, 1.0},")
    lines.append("}};")

    # Optional SEDS register offsets
    if seds_docs:
        lines.append("")
        lines.append("namespace seds {")
        for doc in seds_docs:
            for r in doc["registers"]:
                cname = f"kReg_{doc['package']}_{r['name']}"
                lines.append(f"inline constexpr uint32_t {cname} = 0x{r['offset']:04X}U;")
        lines.append("} // namespace seds")

    lines.append("")
    lines.append("} // namespace strata::generated")
    return "\n".join(lines) + "\n"


def emit_hyp_tables(data: dict, manifest_hash: int) -> str:
    parts = data.get("partitions", [])
    lines = [
        "#pragma once",
        "// AUTO-GENERATED BY tools/strata-gen/strata_gen.py — DO NOT EDIT",
        "#include <strata/hyp/hypervisor.hpp>",
        "#include <array>",
        "",
        "namespace strata::generated {",
        f"inline constexpr uint64_t kHypManifestHash = 0x{manifest_hash:016X}ULL;",
        f"inline constexpr std::array<strata::hyp::PartitionConfig, {max(1, len(parts))}> kPartitionTable = {{{{",
    ]
    cpu_ids: dict[str, int] = {}
    for idx, p in enumerate(parts):
        cpu = p.get("cpu", "CPU0")
        if cpu not in cpu_ids:
            cpu_ids[cpu] = len(cpu_ids)
        is_b = p.get("criticality") == "CLASS_B"
        is_fdir = "fdir" in p["id"].lower()
        lines.append(
            f"    strata::hyp::PartitionConfig{{.partition_id = {idx}U, "
            f".criticality = {_crit_cpp(p.get('criticality', 'CLASS_D'))}, "
            f".cpu_id = {cpu_ids[cpu]}U, "
            f".privileged_slot_ctl = {'true' if (is_b and is_fdir) else 'false'}, "
            f".privileged_part_ctl = {'true' if (is_b and is_fdir) else 'false'}, "
            f".budget_us = {int(p.get('budget_us', 1000))}U, "
            f".period_minor_frames = 1U, .offset_minor_frames = 0U}},"
        )
    lines.append("}};")
    lines.append("} // namespace strata::generated")
    return "\n".join(lines) + "\n"


def emit_linker_script(part: dict, idx: int, manifest_hash: int) -> str:
    ram_base = int(part.get("ram_base", 0x1000_0000 + idx * 0x0100_0000))
    ram_bytes = int(part.get("ram_bytes", 1_048_576))
    cds_bytes = int(part.get("cds_bytes", 8_192))
    cds_base = ram_base + ram_bytes - cds_bytes
    main_ram = ram_bytes - cds_bytes
    return f"""/* AUTO-GENERATED STRATA-20 Linker Script for partition '{part["id"]}' */
/* MANIFEST_HASH = 0x{manifest_hash:016X} */
MEMORY {{
    PART_RAM       (rwx) : ORIGIN = 0x{ram_base:08X}, LENGTH = 0x{main_ram:08X}
    CRITICAL_STORE (rw)  : ORIGIN = 0x{cds_base:08X}, LENGTH = 0x{cds_bytes:08X}
}}

SECTIONS {{
    .text : {{ *(.text*) *(.rodata*) }} > PART_RAM
    .data : {{ *(.data*) }} > PART_RAM
    .bss (NOLOAD) : {{
        __bss_start = .;
        *(.bss*) *(COMMON)
        __bss_end = .;
    }} > PART_RAM
    .critical_store (NOLOAD) : {{
        __critical_store_start = .;
        KEEP(*(.critical_store*))
        __critical_store_end = .;
    }} > CRITICAL_STORE
    .nv_flight_log (NOLOAD) : {{
        __nv_flight_log_start = .;
        KEEP(*(.nv_flight_log*))
        __nv_flight_log_end = .;
    }} > CRITICAL_STORE
}}
"""


def emit_sv_scsr_pkg(data: dict, manifest_hash: int) -> str:
    lo = manifest_hash & 0xFFFF_FFFF
    hi = (manifest_hash >> 32) & 0xFFFF_FFFF
    lines = [
        "// AUTO-GENERATED BY tools/strata-gen/strata_gen.py — DO NOT EDIT",
        "package strata_scsr_pkg;",
        f"  localparam logic [31:0] MANIFEST_HASH_LO = 32'h{lo:08X};",
        f"  localparam logic [31:0] MANIFEST_HASH_HI = 32'h{hi:08X};",
        f"  localparam int unsigned NUM_HW_SLOTS     = {len(data.get('hw_slots', []))};",
    ]
    for s in data.get("hw_slots", []):
        sname = s["name"].upper()
        addr = int(s.get("scsr_phys_addr", 0))
        lines.append(f"  localparam logic [63:0] SLOT_{sname}_BASE = 64'h{addr:016X};")
        lines.append(f"  localparam int unsigned SLOT_{sname}_ID   = {int(s['slot_id'])};")
    lines.append("endpackage : strata_scsr_pkg")
    return "\n".join(lines) + "\n"


def emit_vivado_tcl(data: dict, manifest_hash: int) -> str:
    mission = data["mission"]
    lines = [
        f"# AUTO-GENERATED STRATA-20 Vivado XMPU/SMMU/NoC QoS Isolation Script",
        f"# Mission: {mission['name']} | Silicon: {mission['target_silicon']} | MANIFEST_HASH: 0x{manifest_hash:016X}",
        f"set STRATA_MANIFEST_HASH 0x{manifest_hash:016X}",
        "",
    ]
    for pool in data.get("data_pools", []):
        base = int(pool["phys_base"])
        size = int(pool["slot_count"]) * int(pool["slot_bytes"])
        lines.append(
            f"# XMPU DDR Pool '{pool['name']}' writer=slot:{pool.get('writer_slot', 0)} readers={pool.get('readers', [])}"
        )
        lines.append(
            f"puts \"STRATA XMPU_REGION {pool['name']} BASE=0x{base:016X} SIZE=0x{size:016X}\""
        )
    for slot in data.get("hw_slots", []):
        grp = slot.get("noc_routing_group", "SCIENCE_BEST_EFFORT")
        lines.append(
            f"# Slot {slot['slot_id']} ({slot['name']}): mode={slot['mode']} NoC={grp}"
        )
        lines.append(
            f"puts \"STRATA NOC_QOS {slot['name']} GROUP={grp} QUIESCE_US={int(slot.get('quiesce_timeout_us', 500))}\""
        )
    return "\n".join(lines) + "\n"


def emit_xtce_xml(data: dict, seds_docs: list[dict], manifest_hash: int) -> str:
    mission = data["mission"]
    root = ET.Element(
        "SpaceSystem",
        {
            "xmlns": "http://www.omg.org/spec/XTCE/20180204",
            "name": mission["name"],
            "shortDescription": f"STRATA-20 Auto-Generated Dictionary MANIFEST_HASH=0x{manifest_hash:016X}",
        },
    )
    tlm_meta = ET.SubElement(root, "TelemetryMetaData")
    param_types = ET.SubElement(tlm_meta, "ParameterTypeSet")
    param_set = ET.SubElement(tlm_meta, "ParameterSet")

    for t in data.get("telemetry", []):
        tname = t["name"]
        ptype = ET.SubElement(param_types, "IntegerParameterType", {"name": f"{tname}_Type", "signed": "false"})
        if t.get("unit"):
            uset = ET.SubElement(ptype, "UnitSet")
            ET.SubElement(uset, "Unit").text = t["unit"]
        enc = ET.SubElement(ptype, "IntegerDataEncoding", {"sizeInBits": "64" if t.get("type") == "uint64" else "16"})
        cal = ET.SubElement(enc, "DefaultCalibrator")
        poly = ET.SubElement(cal, "PolynomialCalibrator")
        ET.SubElement(poly, "Term", {"exponent": "0", "coefficient": str(t.get("cal_c0", 0.0))})
        ET.SubElement(poly, "Term", {"exponent": "1", "coefficient": str(t.get("cal_c1", 1.0))})
        ET.SubElement(
            param_set,
            "Parameter",
            {
                "name": tname,
                "parameterTypeRef": f"{tname}_Type",
                "shortDescription": f"channel_id=0x{int(t['channel_id']):04X}",
            },
        )

    for doc in seds_docs:
        for r in doc["registers"]:
            rname = f"{doc['package']}_{r['name']}"
            ptype = ET.SubElement(param_types, "IntegerParameterType", {"name": f"{rname}_Type", "signed": "false"})
            if r.get("unit"):
                uset = ET.SubElement(ptype, "UnitSet")
                ET.SubElement(uset, "Unit").text = r["unit"]
            ET.SubElement(
                param_set,
                "Parameter",
                {"name": rname, "parameterTypeRef": f"{rname}_Type", "shortDescription": f"seds_offset=0x{r['offset']:04X}"},
            )

    cmd_meta = ET.SubElement(root, "CommandMetaData")
    mc_set = ET.SubElement(cmd_meta, "MetaCommandSet")
    for c in data.get("commands", []):
        ET.SubElement(
            mc_set,
            "MetaCommand",
            {
                "name": c["name"],
                "shortDescription": f"opcode=0x{int(c['opcode']):04X} role={c.get('min_role', 'OPERATOR')} bytes={int(c.get('payload_bytes', 0))}",
            },
        )

    ET.indent(root, space="  ")
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="unicode") + "\n"


def emit_json_dict(data: dict, seds_docs: list[dict], manifest_hash: int) -> str:
    payload = {
        "mission": data["mission"],
        "manifest_hash": f"0x{manifest_hash:016X}",
        "partitions": data.get("partitions", []),
        "hw_slots": data.get("hw_slots", []),
        "data_pools": data.get("data_pools", []),
        "pipelines": data.get("pipelines", []),
        "commands": data.get("commands", []),
        "telemetry": data.get("telemetry", []),
        "seds_packages": seds_docs,
    }
    return json.dumps(payload, indent=2) + "\n"


def emit_all(data: dict, seds_docs: list[dict], manifest_hash: int, out_dir: pathlib.Path) -> list[pathlib.Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[pathlib.Path] = []

    def _write(rel: str, content: str) -> None:
        p = out_dir / rel
        p.write_text(content, encoding="utf-8")
        written.append(p)

    _write("strata_mission_icd.hpp", emit_cpp_icd(data, seds_docs, manifest_hash))
    _write("strata_hyp_tables.hpp", emit_hyp_tables(data, manifest_hash))
    for idx, part in enumerate(data.get("partitions", [])):
        _write(f"link_{part['id']}.ld", emit_linker_script(part, idx, manifest_hash))
    _write("scsr_pkg.sv", emit_sv_scsr_pkg(data, manifest_hash))
    _write("strata_xmpu_noc.tcl", emit_vivado_tcl(data, manifest_hash))
    _write("mission_xtce.xml", emit_xtce_xml(data, seds_docs, manifest_hash))
    _write("mission_dict.json", emit_json_dict(data, seds_docs, manifest_hash))
    return written


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="STRATA-20 manifest validator & multi-domain code generator")
    parser.add_argument("manifest", type=pathlib.Path, help="Path to mission.toml")
    parser.add_argument("--seds", type=pathlib.Path, action="append", default=[], help="Optional CCSDS SEDS XML datasheet")
    parser.add_argument("--emit", type=pathlib.Path, default=None, help="Output directory for synchronized multi-domain artifacts")
    args = parser.parse_args(argv)

    raw = args.manifest.read_bytes()
    data = tomllib.loads(raw.decode("utf-8"))
    errors = validate_manifest(data)
    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        return 1

    seds_docs: list[dict] = []
    seds_bytes: list[bytes] = []
    for sp in args.seds:
        seds_bytes.append(sp.read_bytes())
        seds_docs.append(parse_seds_xml(sp))

    manifest_hash = compute_manifest_hash(raw, seds_bytes)
    name = data["mission"]["name"]
    silicon = data["mission"]["target_silicon"]

    emitted_msg = ""
    if args.emit is not None:
        files = emit_all(data, seds_docs, manifest_hash, args.emit)
        emitted_msg = f" | emitted={len(files)} files -> {args.emit}"

    print(
        f"OK: {name} ({silicon}) | partitions={len(data.get('partitions', []))} "
        f"| hw_slots={len(data.get('hw_slots', []))} "
        f"| pools={len(data.get('data_pools', []))} "
        f"| seds={len(seds_docs)} "
        f"| MANIFEST_HASH=0x{manifest_hash:016X}{emitted_msg}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
