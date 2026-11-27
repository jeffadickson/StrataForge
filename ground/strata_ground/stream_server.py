"""Pluggable Telemetry/Command Stream Broker, Handler Pipeline, PCAP Archiver & CSV Exporter."""

from __future__ import annotations

import csv
import dataclasses
import pathlib
import socket
import struct
import threading
import time
from typing import Any, Callable

from .cmd_seq_table import decode_event_record, decode_pus_c_tm
from .dictionary import MissionDictionary
from .limits import LimitMonitor, LimitState


# =============================================================================
# Standard PCAP Binary Stream Archiver (Wireshark-compatible DLT_USER0 = 147)
# =============================================================================
class PcapWriter:
    GLOBAL_HDR = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 147)

    def __init__(self, path: pathlib.Path | str) -> None:
        self._path = pathlib.Path(path)
        self._fh = self._path.open("wb")
        self._fh.write(self.GLOBAL_HDR)

    def write_packet(self, data: bytes, timestamp_ns: int | None = None) -> None:
        ts_ns = time.time_ns() if timestamp_ns is None else timestamp_ns
        sec = ts_ns // 1_000_000_000
        usec = (ts_ns % 1_000_000_000) // 1_000
        self._fh.write(struct.pack("<IIII", sec & 0xFFFF_FFFF, usec & 0xFFFF_FFFF, len(data), len(data)))
        self._fh.write(data)
        self._fh.flush()

    def close(self) -> None:
        self._fh.close()

    def __enter__(self) -> PcapWriter:
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()


class PcapReader:
    """Reads captured timestamped flight wire frames back for deterministic offline replay."""

    def __init__(self, path: pathlib.Path | str) -> None:
        self._path = pathlib.Path(path)

    def read_all(self) -> list[tuple[int, bytes]]:
        raw = self._path.read_bytes()
        if len(raw) < 24:
            raise ValueError("Truncated PCAP file")
        magic = struct.unpack("<I", raw[:4])[0]
        if magic != 0xA1B2C3D4:
            raise ValueError(f"Unsupported PCAP magic: 0x{magic:08X}")
        pos = 24
        packets: list[tuple[int, bytes]] = []
        while pos + 16 <= len(raw):
            sec, usec, incl_len, _ = struct.unpack("<IIII", raw[pos : pos + 16])
            pos += 16
            payload = raw[pos : pos + incl_len]
            pos += incl_len
            packets.append((sec * 1_000_000_000 + usec * 1_000, payload))
        return packets


@dataclasses.dataclass
class TelemetrySample:
    channel: str
    channel_id: int
    raw_value: int
    eng_value: float
    unit: str
    limit_state: LimitState
    timestamp_ns: int


def export_tlm_csv(samples: list[TelemetrySample], out_path: pathlib.Path | str) -> None:
    """Export calibrated telemetry samples and limit states to CSV for post-pass analysis."""
    with pathlib.Path(out_path).open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["timestamp_ns", "channel_id", "channel", "raw_value", "eng_value", "unit", "limit_state"])
        for s in samples:
            w.writerow([s.timestamp_ns, f"0x{s.channel_id:04X}", s.channel, s.raw_value, f"{s.eng_value:.6g}", s.unit, s.limit_state.value])


# =============================================================================
# Pluggable Ground Telemetry/Command Stream Broker & Handler Pipeline
# =============================================================================
class TelemetryBroker:
    """Routes inbound raw telemetry/EVR/PUS-C frames through pluggable handlers, limits, & PCAP."""

    def __init__(self, dictionary: MissionDictionary, limits: LimitMonitor | None = None) -> None:
        self.dict = dictionary
        self.limits = limits if limits is not None else LimitMonitor(dictionary)
        self._lock = threading.Lock()
        self._handlers: list[Callable[[bytes, int], None]] = []
        self._pcap: PcapWriter | None = None

        self.latest_tlm: dict[str, TelemetrySample] = {}
        self.tlm_history: list[TelemetrySample] = []
        self.evr_history: list[dict[str, Any]] = []
        self.pus_tm_history: list[dict[str, Any]] = []
        self.cmd_history: list[dict[str, Any]] = []

        self._udp_sock: socket.socket | None = None
        self._udp_thread: threading.Thread | None = None
        self._stop_evt = threading.Event()

    def add_handler(self, fn: Callable[[bytes, int], None]) -> None:
        self._handlers.append(fn)

    def start_pcap_capture(self, path: pathlib.Path | str) -> None:
        self._pcap = PcapWriter(path)

    def stop_pcap_capture(self) -> None:
        if self._pcap is not None:
            self._pcap.close()
            self._pcap = None

    def start_udp_listener(self, host: str = "127.0.0.1", port: int = 0) -> int:
        """Bind a UDP telemetry ingress socket (port=0 picks an ephemeral OS port) and return port."""
        self._stop_evt.clear()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((host, port))
        sock.settimeout(0.05)
        self._udp_sock = sock
        bound_port = int(sock.getsockname()[1])

        def _loop() -> None:
            while not self._stop_evt.is_set():
                try:
                    data, _ = sock.recvfrom(4096)
                except TimeoutError:
                    continue
                except OSError:
                    break
                self.ingest_raw(data)

        self._udp_thread = threading.Thread(target=_loop, daemon=True)
        self._udp_thread.start()
        return bound_port

    def stop_udp_listener(self) -> None:
        self._stop_evt.set()
        if self._udp_sock is not None:
            self._udp_sock.close()
            self._udp_sock = None
        if self._udp_thread is not None:
            self._udp_thread.join(timeout=1.0)
            self._udp_thread = None

    def record_command(self, name: str, wire_bytes: bytes, timestamp_ns: int | None = None) -> None:
        ts = time.time_ns() if timestamp_ns is None else timestamp_ns
        with self._lock:
            self.cmd_history.append({"timestamp_ns": ts, "command": name, "hex": wire_bytes.hex().upper()})
            if self._pcap is not None:
                self._pcap.write_packet(wire_bytes, ts)

    def ingest_telemetry_channel(self, channel_id: int, raw_value: int, timestamp_ns: int | None = None) -> TelemetrySample:
        ts = time.time_ns() if timestamp_ns is None else timestamp_ns
        tdef = self.dict.telemetry(channel_id)
        eng = tdef.calibrate(raw_value)
        with self._lock:
            lstate = self.limits.evaluate(tdef.name, eng, ts)
            sample = TelemetrySample(
                channel=tdef.name,
                channel_id=tdef.channel_id,
                raw_value=raw_value,
                eng_value=eng,
                unit=tdef.unit,
                limit_state=lstate,
                timestamp_ns=ts,
            )
            self.latest_tlm[tdef.name] = sample
            self.tlm_history.append(sample)
            return sample

    def ingest_event_bytes(self, raw32: bytes) -> dict[str, Any]:
        ev = decode_event_record(raw32)
        with self._lock:
            self.evr_history.append(ev)
        return ev

    def ingest_raw(self, packet: bytes, timestamp_ns: int | None = None) -> None:
        """Process an inbound wire frame: archive to PCAP, dispatch PUS-C TM / EVR / TLM & custom handlers."""
        ts = time.time_ns() if timestamp_ns is None else timestamp_ns
        with self._lock:
            if self._pcap is not None:
                self._pcap.write_packet(packet, ts)

        # Auto-recognize compact StrataGround SIL telemetry frame: b"STTM" + [ch_id:u16][raw:u64]
        if len(packet) == 14 and packet[:4] == b"STTM":
            ch_id, raw_val = struct.unpack(">HQ", packet[4:])
            if ch_id in self.dict.telemetry_by_id:
                self.ingest_telemetry_channel(ch_id, raw_val, ts)
        # Auto-recognize compact StrataGround EVR frame: b"STEV" + 32B EventRecord
        elif len(packet) == 36 and packet[:4] == b"STEV":
            self.ingest_event_bytes(packet[4:])
        # Auto-recognize CCSDS Space Packet carrying PUS-C TM (version=0, TC=0, SecHdr=1, PUS ver=2)
        elif len(packet) >= 21 and (packet[0] & 0xF8) == 0x08 and (packet[6] >> 4) == 2:
            try:
                tm = decode_pus_c_tm(packet)
                with self._lock:
                    self.pus_tm_history.append(tm)
            except ValueError:
                pass

        for h in self._handlers:
            h(packet, ts)
