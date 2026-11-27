"""Automated Flight Integration & Test (I&T) Scripting API (`GroundTestSession`)."""

from __future__ import annotations

import struct
import time
from typing import Callable

from .cmd_seq_table import encode_pus_c_tc, encode_tc_frame
from .dictionary import MissionDictionary
from .stream_server import TelemetryBroker, TelemetrySample


class GroundTestSession:
    """High-level Python test automation API for FlatSat, TVAC, SIL, and Verilator HIL procedures."""

    def __init__(
        self,
        dictionary: MissionDictionary,
        broker: TelemetryBroker,
        uplink_sender: Callable[[bytes], None],
        scid: int = 0x123,
        vcid: int = 1,
    ) -> None:
        self.dict = dictionary
        self.broker = broker
        self._send = uplink_sender
        self.scid = scid
        self.vcid = vcid
        self._tc_seq = 0
        self._pus_seq = 0

    def send_cmd(self, name: str, arg_bytes: bytes = b"") -> bytes:
        """Validate command against MissionDictionary, wrap in CCSDS TC + FECF, log & transmit."""
        cdef = self.dict.command(name)
        if len(arg_bytes) != cdef.payload_bytes:
            raise ValueError(f"Command '{name}' expects {cdef.payload_bytes}B payload, got {len(arg_bytes)}B")
        payload = struct.pack(">H", cdef.opcode) + arg_bytes
        frame = encode_tc_frame(self.scid, self.vcid, self._tc_seq, payload)
        self._tc_seq = (self._tc_seq + 1) & 0xFF
        self.broker.record_command(name, frame)
        self._send(frame)
        return frame

    def send_pus_tc(self, svc: int, sub: int, app_data: bytes = b"", apid: int = 0x105, ack_flags: int = 0x09) -> bytes:
        """Send an ECSS-E-ST-70-41C (PUS-C) Telecommand Space Packet."""
        pkt = encode_pus_c_tc(apid, self._pus_seq, svc, sub, app_data, ack_flags=ack_flags)
        self._pus_seq = (self._pus_seq + 1) & 0x3FFF
        self.broker.record_command(f"PUS_TC({svc},{sub})", pkt)
        self._send(pkt)
        return pkt

    def wait_for_tlm(
        self,
        channel: str,
        predicate: Callable[[TelemetrySample], bool],
        timeout_s: float = 2.0,
        poll_s: float = 0.01,
    ) -> TelemetrySample:
        """Poll until telemetry channel satisfies `predicate(sample)` or raise TimeoutError."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            sample = self.broker.latest_tlm.get(channel)
            if sample is not None and predicate(sample):
                return sample
            time.sleep(poll_s)
        raise TimeoutError(f"Timed out waiting for telemetry '{channel}' predicate after {timeout_s}s")

    def wait_for_evr(self, event_id: int, timeout_s: float = 2.0, poll_s: float = 0.01) -> dict:
        """Poll until a binary EventRecord matching `event_id` arrives or raise TimeoutError."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            for ev in reversed(self.broker.evr_history):
                if ev["event_id"] == event_id:
                    return ev
            time.sleep(poll_s)
        raise TimeoutError(f"Timed out waiting for EVR 0x{event_id:04X} after {timeout_s}s")

    def wait_for_pus_tm(self, svc: int, sub: int, timeout_s: float = 2.0, poll_s: float = 0.01) -> dict:
        """Poll until a PUS-C TM(svc, sub) packet arrives or raise TimeoutError."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            for tm in reversed(self.broker.pus_tm_history):
                if tm["service_type"] == svc and tm["service_subtype"] == sub:
                    return tm
            time.sleep(poll_s)
        raise TimeoutError(f"Timed out waiting for PUS-C TM({svc},{sub}) after {timeout_s}s")

    def assert_limits_nominal(self) -> None:
        if not self.broker.limits.all_nominal():
            bad = {k: v.value for k, v in self.broker.limits._states.items() if v.value != "NOMINAL"}
            raise AssertionError(f"Non-nominal telemetry limits active: {bad}")
