"""CCSDS/PUS-C Codecs, Flight Sequence (.seq) Compiler, and ParamBank Table Compiler."""

from __future__ import annotations

import dataclasses
import struct
from typing import Any

from .dictionary import MissionDictionary


def crc16_ccitt(data: bytes) -> int:
    """CCSDS CRC-16-CCITT (poly 0x1021, init 0xFFFF) matching C++20 `strata::services::comms::crc16_ccitt`."""
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            crc = (((crc << 1) ^ 0x1021) & 0xFFFF) if (crc & 0x8000) else ((crc << 1) & 0xFFFF)
    return crc


def crc32c(data: bytes, init: int = 0) -> int:
    """Castagnoli CRC-32C (poly 0x82F63B78) matching C++20 `strata::Crc32c`."""
    crc = (~init) & 0xFFFF_FFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc >> 1) ^ 0x82F6_3B78) if (crc & 1) else (crc >> 1)
    return (~crc) & 0xFFFF_FFFF


def encode_tc_frame(scid: int, vcid: int, frame_seq: int, data_field: bytes) -> bytes:
    """Encode a CCSDS 232.0-B-4 TC Transfer Frame with trailing CRC-16-CCITT FECF."""
    total = 5 + len(data_field) + 2
    if total > 1024:
        raise ValueError(f"TC frame length {total} exceeds 1024-byte CCSDS maximum")
    w0 = scid & 0x03FF
    w1 = ((vcid & 0x3F) << 10) | ((total - 1) & 0x03FF)
    body = struct.pack(">HHB", w0, w1, frame_seq & 0xFF) + data_field
    return body + struct.pack(">H", crc16_ccitt(body))


def encode_pus_c_tc(
    apid: int,
    seq: int,
    svc: int,
    sub: int,
    app_data: bytes = b"",
    ack_flags: int = 0x09,
    source_id: int = 1,
) -> bytes:
    """Encode an ECSS-E-ST-70-41C (PUS-C) TC Space Packet."""
    pus_hdr = struct.pack(">BBBH", (2 << 4) | (ack_flags & 0x0F), svc & 0xFF, sub & 0xFF, source_id & 0xFFFF)
    payload = pus_hdr + app_data
    w0 = 0x1800 | (apid & 0x07FF)
    w1 = 0xC000 | (seq & 0x3FFF)
    w2 = len(payload) - 1
    return struct.pack(">HHH", w0, w1, w2) + payload


def decode_pus_c_tm(raw_sp: bytes) -> dict[str, Any]:
    """Decode an ECSS-E-ST-70-41C (PUS-C) TM Space Packet."""
    if len(raw_sp) < 6 + 15:
        raise ValueError("Truncated PUS-C TM Space Packet")
    w0, w1, w2 = struct.unpack(">HHH", raw_sp[:6])
    payload = raw_sp[6 : 6 + w2 + 1]
    b0, svc, sub, cnt, dest, tai_ns = struct.unpack(">BBBHHQ", payload[:15])
    if (b0 >> 4) != 2:
        raise ValueError(f"Invalid PUS-C version: {b0 >> 4}")
    return {
        "apid": w0 & 0x07FF,
        "seq_count": w1 & 0x3FFF,
        "service_type": svc,
        "service_subtype": sub,
        "msg_type_counter": cnt,
        "destination_id": dest,
        "cuc_tai_ns": tai_ns,
        "app_data": payload[15:],
        "app_data_hex": payload[15:].hex().upper(),
    }


def decode_event_record(raw32: bytes) -> dict[str, Any]:
    """Decode a 32-byte binary `strata::EventRecord`."""
    if len(raw32) != 32:
        raise ValueError(f"EventRecord must be 32 bytes, got {len(raw32)}")
    first_tai, last_tai, event_id, arg0, arg1, repeat_cnt, sev, part_id = struct.unpack("<QQIIIHBB", raw32)
    sev_names = ["DIAG", "CMD", "ACT_LO", "ACT_HI", "WARN_LO", "WARN_HI", "FATAL"]
    return {
        "first_tai_ns": first_tai,
        "last_tai_ns": last_tai,
        "event_id": event_id,
        "event_id_hex": f"0x{event_id:04X}",
        "arg0": arg0,
        "arg1": arg1,
        "repeat_count": repeat_cnt,
        "severity": sev_names[sev] if sev < len(sev_names) else str(sev),
        "partition_id": part_id,
    }


def decode_nv_log_record(raw56: bytes) -> dict[str, Any]:
    """Decode a 56-byte logical `strata::NvLogRecord` extracted from refresh-free MRAM/FRAM."""
    if len(raw56) != 56:
        raise ValueError(f"NvLogRecord must be 56 bytes, got {len(raw56)}")
    epoch, seq, tai_ns, kind, src_id, code, aux0, aux1, aux2, crc_stored, _ = struct.unpack(
        "<IIQHHIQQQII", raw56
    )
    zeroed = raw56[:48] + b"\x00\x00\x00\x00" + raw56[52:]
    kind_names = {
        1: "RebootMarker",
        2: "CrashTrap",
        3: "CriticalEvent",
        4: "FdirAction",
        5: "DfxTransition",
        6: "SeuAlert",
    }
    return {
        "boot_epoch": epoch,
        "seq_num": seq,
        "tai_ns": tai_ns,
        "kind": kind_names.get(kind, str(kind)),
        "source_id": src_id,
        "code": code,
        "aux0": aux0,
        "aux1": aux1,
        "aux2": aux2,
        "crc_valid": crc32c(zeroed) == crc_stored,
    }


# =============================================================================
# Relative/Absolute Flight Sequence (.seq) Compiler & Decompiler
# Matches `strata::services::cdh::SeqEngine` binary format with CRC-32C trailer.
# =============================================================================
@dataclasses.dataclass(frozen=True)
class SeqStep:
    delay_ms: int
    is_relative: bool
    opcode: int
    command_name: str
    arg_u64: int = 0


class SequenceCompiler:
    """Compiles human-readable `.seq` scripts into CRC-32C binary flight sequences and back."""

    MAGIC = 0x5354_5351  # "STSQ"
    HDR_FMT = "<IQHH"  # magic(4), manifest_hash(8), step_count(2), reserved(2)
    STEP_FMT = "<IBBHQ"  # delay_ms(4), is_relative(1), pad(1), opcode(2), arg_u64(8)

    def __init__(self, dictionary: MissionDictionary) -> None:
        self.dict = dictionary

    def compile_text(self, seq_text: str) -> bytes:
        steps: list[SeqStep] = []
        for lineno, raw_line in enumerate(seq_text.splitlines(), start=1):
            line = raw_line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 2:
                raise ValueError(f"Line {lineno}: expected '<time_s> <CMD_NAME> [arg_int]'")
            time_tok, cmd_name = parts[0], parts[1]
            is_rel = time_tok.startswith("+")
            delay_ms = int(round(float(time_tok[1:] if is_rel else time_tok) * 1000.0))
            if delay_ms < 0:
                raise ValueError(f"Line {lineno}: negative delay not allowed")
            cmd_def = self.dict.command(cmd_name)
            arg_val = int(parts[2], 0) if len(parts) >= 3 else 0
            steps.append(SeqStep(delay_ms, is_rel, cmd_def.opcode, cmd_def.name, arg_val))

        body = bytearray()
        body += struct.pack(self.HDR_FMT, self.MAGIC, self.dict.manifest_hash, len(steps), 0)
        for st in steps:
            body += struct.pack(self.STEP_FMT, st.delay_ms, 1 if st.is_relative else 0, 0, st.opcode, st.arg_u64)
        trailer = struct.pack("<I", crc32c(bytes(body)))
        return bytes(body) + trailer

    def decompile(self, blob: bytes) -> list[SeqStep]:
        hdr_sz = struct.calcsize(self.HDR_FMT)
        step_sz = struct.calcsize(self.STEP_FMT)
        if len(blob) < hdr_sz + 4:
            raise ValueError("Sequence binary too short")
        body, crc_rx = blob[:-4], struct.unpack("<I", blob[-4:])[0]
        if crc32c(body) != crc_rx:
            raise ValueError("Sequence CRC-32C mismatch")
        magic, mhash, count, _ = struct.unpack(self.HDR_FMT, body[:hdr_sz])
        if magic != self.MAGIC:
            raise ValueError(f"Invalid sequence magic: 0x{magic:08X}")
        if mhash != self.dict.manifest_hash:
            raise ValueError(f"Sequence MANIFEST_HASH mismatch: 0x{mhash:016X} != {self.dict.manifest_hash_hex}")
        if len(body) != hdr_sz + count * step_sz:
            raise ValueError("Sequence binary length does not match step_count")

        steps: list[SeqStep] = []
        for i in range(count):
            off = hdr_sz + i * step_sz
            delay_ms, is_rel, _, opcode, arg_u64 = struct.unpack(self.STEP_FMT, body[off : off + step_sz])
            name = self.dict.commands_by_opcode[opcode].name if opcode in self.dict.commands_by_opcode else f"0x{opcode:04X}"
            steps.append(SeqStep(delay_ms, bool(is_rel), opcode, name, arg_u64))
        return steps


# =============================================================================
# Flight Parameter Table (ParamBank) Binary Compiler & Decompiler
# =============================================================================
class ParamTableCompiler:
    """Compiles key-value flight parameter tables into CRC-32C-verified binary ParamBank images."""

    MAGIC = 0x5354_5042  # "STPB"
    HDR_FMT = "<IQII"  # magic(4), manifest_hash(8), version(4), entry_count(4)
    ENTRY_FMT = "<IQ"  # param_id(4), value_u64(8)

    def __init__(self, manifest_hash: int) -> None:
        self.manifest_hash = manifest_hash

    def compile(self, version: int, entries: list[tuple[int, int]]) -> bytes:
        body = bytearray()
        body += struct.pack(self.HDR_FMT, self.MAGIC, self.manifest_hash, version, len(entries))
        for pid, val in entries:
            body += struct.pack(self.ENTRY_FMT, pid & 0xFFFF_FFFF, val & 0xFFFF_FFFF_FFFF_FFFF)
        return bytes(body) + struct.pack("<I", crc32c(bytes(body)))

    def decompile(self, blob: bytes) -> tuple[int, list[tuple[int, int]]]:
        hdr_sz = struct.calcsize(self.HDR_FMT)
        ent_sz = struct.calcsize(self.ENTRY_FMT)
        if len(blob) < hdr_sz + 4:
            raise ValueError("ParamTable binary too short")
        body, crc_rx = blob[:-4], struct.unpack("<I", blob[-4:])[0]
        if crc32c(body) != crc_rx:
            raise ValueError("ParamTable CRC-32C mismatch")
        magic, mhash, version, count = struct.unpack(self.HDR_FMT, body[:hdr_sz])
        if magic != self.MAGIC or mhash != self.manifest_hash:
            raise ValueError("ParamTable magic or MANIFEST_HASH mismatch")
        entries: list[tuple[int, int]] = []
        for i in range(count):
            off = hdr_sz + i * ent_sz
            entries.append(struct.unpack(self.ENTRY_FMT, body[off : off + ent_sz]))
        return version, entries
