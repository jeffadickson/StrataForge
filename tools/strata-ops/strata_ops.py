#!/usr/bin/env python3
"""STRATA-20 Ground Mission Operations CLI & Telemetry Decoder (`strata-ops`).

Consumes auto-generated `mission_dict.json` from `strata-gen` to:
- Encode operator commands into CCSDS TC Transfer Frames with CRC-16-CCITT FECF.
- Decode raw binary telemetry values and apply generated polynomial engineering-
  unit calibration curves (`c0 + c1 * raw`).
- Format compact binary `EventRecord` (32-byte) flight logs into operator text.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode_tc_frame(scid: int, vcid: int, frame_seq: int, data_field: bytes) -> bytes:
    total = 5 + len(data_field) + 2
    if total > 1024:
        raise ValueError(f"TC frame exceeds 1024 bytes: {total}")
    w0 = scid & 0x03FF
    w1 = ((vcid & 0x3F) << 10) | ((total - 1) & 0x03FF)
    body = struct.pack(">HHB", w0, w1, frame_seq & 0xFF) + data_field
    fecf = crc16_ccitt(body)
    return body + struct.pack(">H", fecf)


def encode_command(dictionary: dict, cmd_name: str, arg_bytes: bytes, scid: int = 0x123, vcid: int = 1, seq: int = 0) -> bytes:
    cmd = next((c for c in dictionary.get("commands", []) if c["name"] == cmd_name), None)
    if cmd is None:
        raise KeyError(f"Unknown command '{cmd_name}'")
    expected_len = int(cmd.get("payload_bytes", 0))
    if len(arg_bytes) != expected_len:
        raise ValueError(f"Command '{cmd_name}' expects {expected_len} payload bytes, got {len(arg_bytes)}")
    payload = struct.pack(">H", int(cmd["opcode"])) + arg_bytes
    return encode_tc_frame(scid, vcid, seq, payload)


def calibrate_telemetry(dictionary: dict, channel_name: str, raw_value: int) -> dict:
    tlm = next((t for t in dictionary.get("telemetry", []) if t["name"] == channel_name), None)
    if tlm is None:
        raise KeyError(f"Unknown telemetry channel '{channel_name}'")
    c0 = float(tlm.get("cal_c0", 0.0))
    c1 = float(tlm.get("cal_c1", 1.0))
    eng = c0 + c1 * float(raw_value)
    return {
        "name": channel_name,
        "channel_id": int(tlm["channel_id"]),
        "raw": raw_value,
        "engineering_value": eng,
        "unit": tlm.get("unit", ""),
    }


def decode_event_record(raw32: bytes) -> dict:
    """Decode a 32-byte strata::EventRecord binary struct (little-endian host/ARM)."""
    if len(raw32) != 32:
        raise ValueError(f"EventRecord must be exactly 32 bytes, got {len(raw32)}")
    first_tai, last_tai, event_id, arg0, arg1, repeat_cnt, sev, part_id = struct.unpack("<QQIIIHBB", raw32)
    sev_names = ["DIAG", "CMD", "ACT_LO", "ACT_HI", "WARN_LO", "WARN_HI", "FATAL"]
    return {
        "first_tai_ns": first_tai,
        "last_tai_ns": last_tai,
        "event_id": f"0x{event_id:04X}",
        "arg0": f"0x{arg0:08X}",
        "arg1": f"0x{arg1:08X}",
        "repeat_count": repeat_cnt,
        "severity": sev_names[sev] if sev < len(sev_names) else str(sev),
        "partition_id": part_id,
    }


def encode_pus_c_tc(apid: int, seq: int, svc: int, sub: int, app_data: bytes = b"",
                    ack_flags: int = 0x09, source_id: int = 1) -> bytes:
    """Encode a CCSDS Space Packet carrying an ECSS-E-ST-70-41C (PUS-C) TC secondary header."""
    pus_hdr = struct.pack(">BBBH", (2 << 4) | (ack_flags & 0x0F), svc & 0xFF, sub & 0xFF, source_id & 0xFFFF)
    payload = pus_hdr + app_data
    w0 = 0x1800 | (apid & 0x07FF)  # TC=1, SecHdr=1
    w1 = 0xC000 | (seq & 0x3FFF)   # Unsegmented
    w2 = len(payload) - 1
    return struct.pack(">HHH", w0, w1, w2) + payload


def decode_pus_c_tm(raw_sp: bytes) -> dict:
    """Decode a CCSDS Space Packet carrying an ECSS-E-ST-70-41C (PUS-C) TM secondary header."""
    if len(raw_sp) < 6 + 15:
        raise ValueError("Truncated PUS-C TM Space Packet")
    w0, w1, w2 = struct.unpack(">HHH", raw_sp[:6])
    payload = raw_sp[6:6 + w2 + 1]
    b0, svc, sub, cnt, dest, tai_ns = struct.unpack(">BBBHHQ", payload[:15])
    if (b0 >> 4) != 2:
        raise ValueError(f"Expected PUS-C version 2, got {b0 >> 4}")
    return {
        "apid": w0 & 0x07FF,
        "seq_count": w1 & 0x3FFF,
        "service_type": svc,
        "service_subtype": sub,
        "msg_type_counter": cnt,
        "destination_id": dest,
        "cuc_tai_ns": tai_ns,
        "app_data_hex": payload[15:].hex().upper(),
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="STRATA-20 Ground Operations CLI & Decoder")
    parser.add_argument("--dict", type=pathlib.Path, required=True, help="Path to mission_dict.json")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_enc = sub.add_parser("encode-cmd", help="Encode a named command into a CCSDS TC frame (hex)")
    p_enc.add_argument("name")
    p_enc.add_argument("--arg-hex", default="")
    p_enc.add_argument("--scid", type=lambda s: int(s, 0), default=0x123)
    p_enc.add_argument("--vcid", type=int, default=1)
    p_enc.add_argument("--seq", type=int, default=0)

    p_cal = sub.add_parser("calibrate", help="Calibrate a raw telemetry sample to engineering units")
    p_cal.add_argument("channel")
    p_cal.add_argument("raw", type=lambda s: int(s, 0))

    p_pus = sub.add_parser("encode-pus-tc", help="Encode an ECSS-E-ST-70-41C PUS-C TC Space Packet (hex)")
    p_pus.add_argument("--apid", type=lambda s: int(s, 0), default=0x105)
    p_pus.add_argument("--seq", type=int, default=1)
    p_pus.add_argument("--svc", type=int, required=True)
    p_pus.add_argument("--sub", type=int, required=True)
    p_pus.add_argument("--app-hex", default="")

    args = parser.parse_args(argv)
    dictionary = json.loads(args.dict.read_text(encoding="utf-8"))

    if args.cmd == "encode-cmd":
        frame = encode_command(dictionary, args.name, bytes.fromhex(args.arg_hex), args.scid, args.vcid, args.seq)
        print(frame.hex().upper())
        return 0

    if args.cmd == "calibrate":
        out = calibrate_telemetry(dictionary, args.channel, args.raw)
        print(json.dumps(out))
        return 0

    if args.cmd == "encode-pus-tc":
        pkt = encode_pus_c_tc(args.apid, args.seq, args.svc, args.sub, bytes.fromhex(args.app_hex))
        print(pkt.hex().upper())
        return 0

    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
