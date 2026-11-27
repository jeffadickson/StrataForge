#!/usr/bin/env python3
"""Live Closed-Loop Flight-to-Ground UDP Integration Test (STRATA-VER-007).

Launches the real compiled C++20 `live_ground_sil` flight executable (running
`HyperspectralImagerMock`, `PusCAdapter`, `FdirEngine`, `NvFaultLog`, and
`FramePool`) and drives it over live loopback UDP from `StrataGround`:
1. Verifies live telemetry streaming (`CDH_UPTIME_MS`, `HSI_DETECTOR_TEMP_K`).
2. Sends CCSDS TC `HSI_SET_INTEGRATION` and PUS-C `ST[17,1]` Are-You-Alive ->
   verifies live `ST[17,2]` + `ST[01,7]` + `EVR` responses over UDP.
3. Injects a live `ThermalRunaway` fault via PUS-C `ST[08,1]` -> observes real
   thermal physics heat `HSI_DETECTOR_TEMP_K` past Yellow (295 K) and Red (310 K)
   limits in `StrataGround`'s `LimitMonitor` with persistence debouncing!
4. Observes autonomous Flight FDIR trip (`EVR 0x0201` + `PUS-C TM(5,3)`),
   instrument safing, and `NvFaultLog` persistence.
5. Sends graceful stop command and verifies Wireshark `.pcap` & `.csv` outputs.
"""

from __future__ import annotations

import argparse
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
GEN_SCRIPT = REPO_ROOT / "tools" / "strata-gen" / "strata_gen.py"
VERSAL_TOML = REPO_ROOT / "manifests" / "examples" / "versal_dual_instrument.toml"
SEDS_XML = REPO_ROOT / "manifests" / "examples" / "hyperspec_sensor_seds.xml"

sys.path.insert(0, str(REPO_ROOT / "ground"))
from strata_ground import (  # noqa: E402
    GroundTestSession,
    LimitMonitor,
    LimitState,
    MissionDictionary,
    PcapReader,
    TelemetryBroker,
    export_tlm_csv,
)


def pick_free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Live UDP Flight-to-Ground I&T verification")
    parser.add_argument("flight_bin", type=pathlib.Path, help="Path to compiled live_ground_sil executable")
    args = parser.parse_args(argv)

    with tempfile.TemporaryDirectory(prefix="strata_live_loop_") as td_str:
        td = pathlib.Path(td_str)
        gen_dir = td / "gen"

        # 1. Generate live mission_dict.json directly from versal_dual_instrument.toml
        gen = subprocess.run(
            [sys.executable, str(GEN_SCRIPT), str(VERSAL_TOML), "--seds", str(SEDS_XML), "--emit", str(gen_dir)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert gen.returncode == 0, gen.stderr

        mdict = MissionDictionary.from_file(gen_dir / "mission_dict.json")
        limits = LimitMonitor(mdict)
        broker = TelemetryBroker(mdict, limits)
        pcap_path = td / "live_pass.pcap"
        csv_path = td / "live_tlm.csv"
        broker.start_pcap_capture(pcap_path)

        ground_rx_port = broker.start_udp_listener("127.0.0.1", 0)
        flight_rx_port = pick_free_udp_port()

        tx_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        session = GroundTestSession(
            mdict,
            broker,
            uplink_sender=lambda wire: tx_sock.sendto(wire, ("127.0.0.1", flight_rx_port)),
        )

        # 2. Launch the compiled C++20 flight software + HyperspectralImagerMock!
        proc = subprocess.Popen(
            [
                str(args.flight_bin),
                "--rx-port",
                str(flight_rx_port),
                "--tx-port",
                str(ground_rx_port),
                "--ticks",
                "800",
                "--tick-ms",
                "10",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        try:
            # Wait for live nominal detector telemetry over UDP
            s0 = session.wait_for_tlm("HSI_DETECTOR_TEMP_K", lambda s: 240.0 <= s.eng_value <= 290.0, timeout_s=3.0)
            assert s0.limit_state == LimitState.NOMINAL

            # Send CCSDS TC HSI_SET_INTEGRATION = 7500 us -> wait for EVR 0x0101
            session.send_cmd("HSI_SET_INTEGRATION", struct.pack(">I", 7500))
            ev_cmd = session.wait_for_evr(0x0101, timeout_s=2.0)
            assert ev_cmd["arg1"] == 7500

            # Send PUS-C ST[17,1] Are-You-Alive -> wait for live TM(17,2) & ST[01,7]
            session.send_pus_tc(17, 1)
            tm17 = session.wait_for_pus_tm(17, 2, timeout_s=2.0)
            assert tm17["service_type"] == 17 and tm17["service_subtype"] == 2
            tm01 = session.wait_for_pus_tm(1, 7, timeout_s=2.0)
            assert tm01["service_type"] == 1 and tm01["service_subtype"] == 7

            # Inject live ThermalRunaway (+260 W) via PUS-C ST[08,1] (func_id = 0x8001)
            session.send_pus_tc(8, 1, struct.pack(">HI", 0x8001, 260))

            # Wait for live physics to drive HSI_DETECTOR_TEMP_K into Red limit (>310 K)
            hot = session.wait_for_tlm("HSI_DETECTOR_TEMP_K", lambda s: s.eng_value >= 310.0, timeout_s=4.0)
            assert hot.limit_state in (LimitState.WARN_HI, LimitState.ERROR_HI)

            # Wait for Flight FDIR autonomous safing EVR (0x0201) + PUS-C ST[05,3] anomaly report!
            ev_fdir = session.wait_for_evr(0x0201, timeout_s=3.0)
            assert ev_fdir["event_id"] == 0x0201
            tm05 = session.wait_for_pus_tm(5, 3, timeout_s=2.0)
            assert tm05["service_type"] == 5 and tm05["service_subtype"] == 3

            # Verify LimitMonitor recorded Warning/Error transitions
            assert len(limits.alert_history) >= 1

            # Request graceful shutdown via PUS-C ST[08,1] func_id = 0x80FF
            session.send_pus_tc(8, 1, struct.pack(">HI", 0x80FF, 0))
            stdout, stderr = proc.communicate(timeout=4.0)
            assert proc.returncode == 0, f"live_ground_sil failed: {stderr}"
            assert "nv_log_records=2" in stdout or "nv_log_records=1" in stdout, stdout
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            broker.stop_udp_listener()
            broker.stop_pcap_capture()
            tx_sock.close()

        # Verify PCAP archival & CSV export of the live flight pass
        pcap_pkts = PcapReader(pcap_path).read_all()
        assert len(pcap_pkts) >= 20
        export_tlm_csv(broker.tlm_history, csv_path)
        assert csv_path.stat().st_size > 200

        print(
            f"OK: Live Flight-to-Ground UDP Loop PASSED | pcap_packets={len(pcap_pkts)} "
            f"| tlm_samples={len(broker.tlm_history)} | limit_alerts={len(limits.alert_history)} "
            f"| pus_tm={len(broker.pus_tm_history)} | evrs={len(broker.evr_history)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
