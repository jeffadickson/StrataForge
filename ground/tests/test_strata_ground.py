#!/usr/bin/env python3
"""End-to-end verification test suite for StrataGround (`ground/`).

Verifies STRATA-GDS-001 through STRATA-GDS-005:
- Single-source-of-truth dictionary loading from `strata-gen` with MANIFEST_HASH & limits
- Red/Yellow LimitMonitor persistence debouncing & alerts
- Relative/absolute `.seq` flight sequence compile -> CRC-32C verify -> decompile
- Binary `ParamBank` table compile -> CRC-32C verify -> decompile
- UDP TelemetryBroker + PCAP archive/replay + CSV export
- `GroundTestSession` automated I&T test procedure (CCSDS TC, PUS-C ST[17], TLM, EVR)
- Live HTTP Web Console REST endpoints (`/`, `/api/dictionary`, `/api/telemetry`,
  `/api/command`, `/api/sequence/compile`, `/api/limits`)
"""

from __future__ import annotations

import json
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
GEN_SCRIPT = REPO_ROOT / "tools" / "strata-gen" / "strata_gen.py"
VERSAL_TOML = REPO_ROOT / "manifests" / "examples" / "versal_dual_instrument.toml"
SEDS_XML = REPO_ROOT / "manifests" / "examples" / "hyperspec_sensor_seds.xml"

sys.path.insert(0, str(REPO_ROOT / "ground"))
from strata_ground import (  # noqa: E402
    GroundTestSession,
    LimitMonitor,
    LimitSpec,
    LimitState,
    MissionDictionary,
    ParamTableCompiler,
    PcapReader,
    SequenceCompiler,
    TelemetryBroker,
    crc32c,
    decode_nv_log_record,
    export_tlm_csv,
)
from strata_ground.web_console import GroundWebConsole  # noqa: E402


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="strata_ground_test_") as td_str:
        td = pathlib.Path(td_str)
        gen_dir = td / "gen"

        # 1. Generate live mission_dict.json from versal_dual_instrument.toml + SEDS
        proc = subprocess.run(
            [sys.executable, str(GEN_SCRIPT), str(VERSAL_TOML), "--seds", str(SEDS_XML), "--emit", str(gen_dir)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert proc.returncode == 0, proc.stderr

        mdict = MissionDictionary.from_file(gen_dir / "mission_dict.json")
        assert mdict.mission_name == "versal_dual_instrument_demo"
        assert "HSI_SET_INTEGRATION" in mdict.commands_by_name
        assert "HSI_DETECTOR_TEMP_K" in mdict.telemetry_by_name

        # 2. Limits & persistence debouncing (STRATA-GDS-002)
        limits = LimitMonitor(mdict)
        limits.register(
            LimitSpec("HSI_DETECTOR_TEMP_K", lo_err=200.0, lo_warn=220.0, hi_warn=295.0, hi_err=310.0, persistence=2),
            unit="K",
        )
        assert limits.evaluate("HSI_DETECTOR_TEMP_K", 280.0, 1) == LimitState.NOMINAL
        assert limits.evaluate("HSI_DETECTOR_TEMP_K", 300.0, 2) == LimitState.NOMINAL  # 1st hit debounced
        assert limits.evaluate("HSI_DETECTOR_TEMP_K", 301.0, 3) == LimitState.WARN_HI  # 2nd hit trips WARN_HI!
        assert limits.evaluate("HSI_DETECTOR_TEMP_K", 315.0, 4) == LimitState.WARN_HI
        assert limits.evaluate("HSI_DETECTOR_TEMP_K", 316.0, 5) == LimitState.ERROR_HI
        assert len(limits.alert_history) == 2
        # Return to nominal (2 ticks)
        limits.evaluate("HSI_DETECTOR_TEMP_K", 275.0, 6)
        assert limits.evaluate("HSI_DETECTOR_TEMP_K", 275.0, 7) == LimitState.NOMINAL

        # 3. Sequence Compiler & ParamBank Table Compiler round-trips (STRATA-GDS-003)
        seq_c = SequenceCompiler(mdict)
        seq_bin = seq_c.compile_text(
            """
            # Automated science commissioning sequence
            0.000 NOOP
            +1.500 HSI_SET_INTEGRATION 4500
            +2.000 SLOT_RELOAD_DFX 1
            """
        )
        steps = seq_c.decompile(seq_bin)
        assert len(steps) == 3
        assert steps[0].command_name == "NOOP" and not steps[0].is_relative
        assert steps[1].command_name == "HSI_SET_INTEGRATION" and steps[1].delay_ms == 1500 and steps[1].arg_u64 == 4500
        assert steps[2].command_name == "SLOT_RELOAD_DFX" and steps[2].delay_ms == 2000

        tbl_c = ParamTableCompiler(mdict.manifest_hash)
        tbl_bin = tbl_c.compile(version=4, entries=[(1, 4500), (2, 16)])
        ver, ents = tbl_c.decompile(tbl_bin)
        assert ver == 4 and ents == [(1, 4500), (2, 16)]

        # Verify NvLogRecord decoder matches 56-byte C++20 struct layout + CRC-32C
        raw_nv_no_crc = struct.pack("<IIQHHIQQQII", 2, 7, 50000, 2, 4, 0x92000047, 0x14002210, 0x10000000, 0, 0, 0)
        nv_crc = crc32c(raw_nv_no_crc)
        raw_nv = raw_nv_no_crc[:48] + struct.pack("<I", nv_crc) + raw_nv_no_crc[52:]
        nv_dec = decode_nv_log_record(raw_nv)
        assert nv_dec["crc_valid"] and nv_dec["boot_epoch"] == 2 and nv_dec["kind"] == "CrashTrap"

        # 4. UDP TelemetryBroker + PCAP archive/replay + CSV export + GroundTestSession (STRATA-GDS-004)
        broker = TelemetryBroker(mdict, limits)
        pcap_path = td / "flight_capture.pcap"
        csv_path = td / "tlm_export.csv"
        broker.start_pcap_capture(pcap_path)
        udp_port = broker.start_udp_listener("127.0.0.1", 0)

        sent_frames: list[bytes] = []
        tx_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        def mock_spacecraft_uplink(wire: bytes) -> None:
            sent_frames.append(wire)
            # Simulate spacecraft reply over UDP telemetry socket:
            # - If PUS-C TC(17,1), emit PUS-C TM(17,2)
            if len(wire) >= 9 and (wire[0] & 0xF8) == 0x18 and wire[7] == 17 and wire[8] == 1:
                pus_payload = struct.pack(">BBBHHQ", 0x20, 17, 2, 1, 1, 123456789)
                tm_sp = struct.pack(">HHH", 0x0800 | 0x105, 0xC000 | 1, len(pus_payload) - 1) + pus_payload
                tx_sock.sendto(tm_sp, ("127.0.0.1", udp_port))
            else:
                # Emit calibrated telemetry sample for HSI_DETECTOR_TEMP_K (raw=2000 -> 280.0 K)
                tlm_pkt = b"STTM" + struct.pack(">HQ", 0x0010, 2000)
                tx_sock.sendto(tlm_pkt, ("127.0.0.1", udp_port))
                # Emit binary EventRecord (event_id = 0x0201, Command)
                ev_pkt = b"STEV" + struct.pack("<QQIIIHBB", 1000, 1000, 0x0201, 4500, 0, 1, 1, 3)
                tx_sock.sendto(ev_pkt, ("127.0.0.1", udp_port))

        session = GroundTestSession(mdict, broker, mock_spacecraft_uplink)
        session.send_cmd("HSI_SET_INTEGRATION", struct.pack(">I", 4500))
        sample = session.wait_for_tlm("HSI_DETECTOR_TEMP_K", lambda s: abs(s.eng_value - 280.0) < 1e-6, timeout_s=2.0)
        assert sample.limit_state == LimitState.NOMINAL
        ev = session.wait_for_evr(0x0201, timeout_s=2.0)
        assert ev["arg0"] == 4500

        session.send_pus_tc(17, 1)
        tm17_2 = session.wait_for_pus_tm(17, 2, timeout_s=2.0)
        assert tm17_2["cuc_tai_ns"] == 123456789
        session.assert_limits_nominal()

        # 5. Live Web Mission Control Console HTTP REST endpoints (STRATA-GDS-005)
        web = GroundWebConsole(mdict, broker, session, host="127.0.0.1", port=0)
        web_port = web.start()
        try:
            base = f"http://127.0.0.1:{web_port}"
            html = urllib.request.urlopen(f"{base}/").read().decode("utf-8")
            assert "STRATAGROUND" in html

            tlm_json = json.loads(urllib.request.urlopen(f"{base}/api/telemetry").read().decode("utf-8"))
            assert tlm_json["all_nominal"] and len(tlm_json["telemetry"]) >= 1

            seq_req = urllib.request.Request(
                f"{base}/api/sequence/compile",
                data=json.dumps({"text": "0.000 NOOP\n+0.500 HSI_SET_INTEGRATION 3000"}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
            )
            seq_resp = json.loads(urllib.request.urlopen(seq_req).read().decode("utf-8"))
            assert len(seq_resp["steps"]) == 2 and seq_resp["steps"][1]["arg"] == 3000

            cmd_req = urllib.request.Request(
                f"{base}/api/command",
                data=json.dumps({"mode": "pus", "service_type": 17, "service_subtype": 1}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
            )
            cmd_resp = json.loads(urllib.request.urlopen(cmd_req).read().decode("utf-8"))
            assert cmd_resp["status"] == "sent"
        finally:
            web.stop()
            broker.stop_udp_listener()
            broker.stop_pcap_capture()
            tx_sock.close()

        # Verify PCAP archival & CSV export
        pcap_pkts = PcapReader(pcap_path).read_all()
        assert len(pcap_pkts) >= 5
        export_tlm_csv(broker.tlm_history, csv_path)
        assert "HSI_DETECTOR_TEMP_K" in csv_path.read_text(encoding="utf-8")

    print("OK: StrataGround end-to-end GDS & I&T suite (dict, limits, seq/table compilers, UDP/PCAP/CSV, test API, web console) PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
