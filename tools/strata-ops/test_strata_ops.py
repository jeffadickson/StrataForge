#!/usr/bin/env python3
"""Verification test for STRATA-20 ground mission operations CLI (`strata_ops.py`)."""

from __future__ import annotations

import json
import pathlib
import struct
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
GEN_SCRIPT = REPO_ROOT / "tools" / "strata-gen" / "strata_gen.py"
OPS_SCRIPT = REPO_ROOT / "tools" / "strata-ops" / "strata_ops.py"
VERSAL_TOML = REPO_ROOT / "manifests" / "examples" / "versal_dual_instrument.toml"
SEDS_XML = REPO_ROOT / "manifests" / "examples" / "hyperspec_sensor_seds.xml"

sys.path.insert(0, str(REPO_ROOT / "tools" / "strata-ops"))
import strata_ops  # noqa: E402


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="strata_ops_test_") as td:
        out_dir = pathlib.Path(td) / "gen"
        p = subprocess.run(
            [sys.executable, str(GEN_SCRIPT), str(VERSAL_TOML), "--seds", str(SEDS_XML), "--emit", str(out_dir)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert p.returncode == 0, p.stderr
        dict_path = out_dir / "mission_dict.json"

        # 1. Encode HSI_SET_INTEGRATION (opcode 0x0201, 4-byte arg = 4500 us -> 00001194)
        enc = subprocess.run(
            [sys.executable, str(OPS_SCRIPT), "--dict", str(dict_path), "encode-cmd",
             "HSI_SET_INTEGRATION", "--arg-hex", "00001194", "--seq", "7"],
            capture_output=True,
            text=True,
            check=False,
        )
        assert enc.returncode == 0, enc.stderr
        raw_tc = bytes.fromhex(enc.stdout.strip())
        # Verify TC Frame length (5 hdr + 2 opcode + 4 arg + 2 FECF = 13 bytes) and CRC-16 FECF
        assert len(raw_tc) == 13
        body, fecf = raw_tc[:-2], struct.unpack(">H", raw_tc[-2:])[0]
        assert strata_ops.crc16_ccitt(body) == fecf
        assert raw_tc[4] == 7
        assert struct.unpack(">HI", raw_tc[5:11]) == (0x0201, 4500)

        # 2. Calibrate HSI_DETECTOR_TEMP_K: raw=2000 -> 180.0 + 0.05*2000 = 280.0 K
        cal = subprocess.run(
            [sys.executable, str(OPS_SCRIPT), "--dict", str(dict_path), "calibrate",
             "HSI_DETECTOR_TEMP_K", "2000"],
            capture_output=True,
            text=True,
            check=False,
        )
        assert cal.returncode == 0, cal.stderr
        cal_res = json.loads(cal.stdout)
        assert abs(cal_res["engineering_value"] - 280.0) < 1e-6 and cal_res["unit"] == "K"

        # 3. Decode a 32-byte binary EventRecord
        raw_ev = struct.pack("<QQIIIHBB", 1000, 2000, 0x0042, 0x11, 0x22, 3, 6, 1)
        ev = strata_ops.decode_event_record(raw_ev)
        assert ev["event_id"] == "0x0042" and ev["severity"] == "FATAL" and ev["repeat_count"] == 3

    print("OK: strata_ops ground CLI roundtrip (CCSDS TC FECF + engineering calibration + EventRecord) PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
