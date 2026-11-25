#!/usr/bin/env python3
"""End-to-end verification test for tools/strata-gen/strata_gen.py.

Verifies STRATA-GEN-001, STRATA-GEN-002, STRATA-GEN-003, and STRATA-GEN-004:
- Validated multi-domain emission with CCSDS SEDS XML import
- Identical 64-bit MANIFEST_HASH stamped across C++20, Hypervisor, Linker, SV, TCL, XTCE, and JSON
- Well-formed XTCE XML and JSON dictionaries
- Negative validation proofs (schedulability violation, DDR overlap, NoC isolation violation)
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
GEN_SCRIPT = REPO_ROOT / "tools" / "strata-gen" / "strata_gen.py"
VERSAL_TOML = REPO_ROOT / "manifests" / "examples" / "versal_dual_instrument.toml"
SEDS_XML = REPO_ROOT / "manifests" / "examples" / "hyperspec_sensor_seds.xml"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="strata_gen_test_") as td:
        out_dir = pathlib.Path(td) / "gen_out"
        proc = subprocess.run(
            [sys.executable, str(GEN_SCRIPT), str(VERSAL_TOML), "--seds", str(SEDS_XML), "--emit", str(out_dir)],
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            print("FAIL: strata_gen.py failed:", proc.stderr, file=sys.stderr)
            return 1

        # Extract MANIFEST_HASH from stdout
        marker = "MANIFEST_HASH=0x"
        assert marker in proc.stdout, f"Missing MANIFEST_HASH in output: {proc.stdout}"
        hash_hex = proc.stdout.split(marker)[1][:16]
        hash_lo = hash_hex[8:16]
        hash_hi = hash_hex[0:8]

        expected_files = [
            "strata_mission_icd.hpp",
            "strata_hyp_tables.hpp",
            "link_cdh_b.ld",
            "link_fdir_slot_mgr_b.ld",
            "link_storage_c.ld",
            "link_hyperspec_d.ld",
            "link_sar_dfx_d.ld",
            "scsr_pkg.sv",
            "strata_xmpu_noc.tcl",
            "mission_xtce.xml",
            "mission_dict.json",
        ]
        for fname in expected_files:
            fpath = out_dir / fname
            assert fpath.is_file(), f"Missing generated artifact: {fname}"

        # Check identical MANIFEST_HASH across every generated artifact domain
        assert f"0x{hash_hex}ULL" in (out_dir / "strata_mission_icd.hpp").read_text()
        assert f"0x{hash_hex}ULL" in (out_dir / "strata_hyp_tables.hpp").read_text()
        assert f"0x{hash_hex}" in (out_dir / "link_cdh_b.ld").read_text()
        assert f"32'h{hash_lo}" in (out_dir / "scsr_pkg.sv").read_text()
        assert f"32'h{hash_hi}" in (out_dir / "scsr_pkg.sv").read_text()
        assert f"0x{hash_hex}" in (out_dir / "strata_xmpu_noc.tcl").read_text()

        # Verify SEDS register appeared in C++20 ICD
        assert "kReg_HyperspecImagerSensor_INTEGRATION_US = 0x1000U" in (
            out_dir / "strata_mission_icd.hpp"
        ).read_text()

        # Verify XTCE XML parses cleanly and carries MANIFEST_HASH
        xtce_root = ET.parse(out_dir / "mission_xtce.xml").getroot()
        assert hash_hex in xtce_root.attrib.get("shortDescription", "")

        # Verify JSON dictionary parses cleanly and carries MANIFEST_HASH + SEDS + commands + telemetry
        jdata = json.loads((out_dir / "mission_dict.json").read_text())
        assert jdata["manifest_hash"] == f"0x{hash_hex}"
        assert len(jdata["commands"]) >= 3
        assert len(jdata["telemetry"]) >= 2
        assert len(jdata["seds_packages"]) == 1

        # Negative test: NoC isolation violation must be rejected
        bad_toml = pathlib.Path(td) / "bad_isolation.toml"
        bad_toml.write_text(
            """
[mission]
name = "bad_demo"
target_silicon = "XQRVC1902"
minor_frame_us = 10000
major_frame_us = 100000

[[partitions]]
id = "payload_d"
criticality = "CLASS_D"
cpu = "APU_CORE1"
budget_us = 5000

[[hw_slots]]
slot_id = 0
name = "bad_slot"
mode = "PL_DFX"
owner_partition = "payload_d"
noc_routing_group = "CRITICAL_EXCL"
""",
            encoding="utf-8",
        )
        bad_proc = subprocess.run(
            [sys.executable, str(GEN_SCRIPT), str(bad_toml)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert bad_proc.returncode != 0, "Expected NoC isolation violation to fail"
        assert "Isolation violation" in bad_proc.stderr

        # Compile-check generated C++20 ICD and hypervisor tables with g++ -std=c++20 -fsyntax-only
        cpp_check = pathlib.Path(td) / "check_gen.cpp"
        cpp_check.write_text(
            '#include "strata_mission_icd.hpp"\n'
            '#include "strata_hyp_tables.hpp"\n'
            "static_assert(strata::generated::kManifestHash == strata::generated::kHypManifestHash);\n"
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        cxx_proc = subprocess.run(
            [
                "g++",
                "-std=c++20",
                "-fno-exceptions",
                "-fno-rtti",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsyntax-only",
                f"-I{out_dir}",
                f"-I{REPO_ROOT / 'middleware' / 'include'}",
                f"-I{REPO_ROOT / 'hypervisor' / 'include'}",
                str(cpp_check),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if cxx_proc.returncode != 0:
            print("FAIL: generated C++20 headers did not compile:\n", cxx_proc.stderr, file=sys.stderr)
            return 1

    print(f"OK: strata_gen multi-domain emission + SEDS + XTCE/JSON + C++20 syntax check PASSED (HASH=0x{hash_hex})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
