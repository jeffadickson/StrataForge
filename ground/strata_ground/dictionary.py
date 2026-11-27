"""Single-source-of-truth Mission Dictionary loader for StrataGround."""

from __future__ import annotations

import dataclasses
import json
import pathlib
from typing import Any


@dataclasses.dataclass(frozen=True)
class CommandDef:
    opcode: int
    name: str
    owner_partition: str
    min_role: str = "OPERATOR"
    payload_bytes: int = 0


@dataclasses.dataclass(frozen=True)
class TelemetryDef:
    channel_id: int
    name: str
    owner_partition: str
    dtype: str = "uint32"
    unit: str = ""
    cal_c0: float = 0.0
    cal_c1: float = 1.0
    limit_lo_err: float | None = None
    limit_lo_warn: float | None = None
    limit_hi_warn: float | None = None
    limit_hi_err: float | None = None

    def calibrate(self, raw: int | float) -> float:
        return self.cal_c0 + self.cal_c1 * float(raw)


class MissionDictionary:
    """Loads `mission_dict.json` emitted by `tools/strata-gen/strata_gen.py`."""

    def __init__(self, data: dict[str, Any]) -> None:
        self.raw = data
        self.mission_name: str = str(data["mission"]["name"])
        self.target_silicon: str = str(data["mission"]["target_silicon"])
        self.manifest_hash_hex: str = str(data["manifest_hash"])
        self.manifest_hash: int = int(self.manifest_hash_hex, 16)
        self.partitions: list[dict[str, Any]] = list(data.get("partitions", []))
        self.hw_slots: list[dict[str, Any]] = list(data.get("hw_slots", []))

        self.commands_by_name: dict[str, CommandDef] = {}
        self.commands_by_opcode: dict[int, CommandDef] = {}
        for c in data.get("commands", []):
            cd = CommandDef(
                opcode=int(c["opcode"]),
                name=str(c["name"]),
                owner_partition=str(c["owner_partition"]),
                min_role=str(c.get("min_role", "OPERATOR")),
                payload_bytes=int(c.get("payload_bytes", 0)),
            )
            self.commands_by_name[cd.name] = cd
            self.commands_by_opcode[cd.opcode] = cd

        self.telemetry_by_name: dict[str, TelemetryDef] = {}
        self.telemetry_by_id: dict[int, TelemetryDef] = {}
        for t in data.get("telemetry", []):
            td = TelemetryDef(
                channel_id=int(t["channel_id"]),
                name=str(t["name"]),
                owner_partition=str(t["owner_partition"]),
                dtype=str(t.get("type", "uint32")),
                unit=str(t.get("unit", "")),
                cal_c0=float(t.get("cal_c0", 0.0)),
                cal_c1=float(t.get("cal_c1", 1.0)),
                limit_lo_err=_opt_float(t.get("limit_lo_err")),
                limit_lo_warn=_opt_float(t.get("limit_lo_warn")),
                limit_hi_warn=_opt_float(t.get("limit_hi_warn")),
                limit_hi_err=_opt_float(t.get("limit_hi_err")),
            )
            self.telemetry_by_name[td.name] = td
            self.telemetry_by_id[td.channel_id] = td

    @classmethod
    def from_file(cls, path: pathlib.Path | str) -> MissionDictionary:
        data = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
        return cls(data)

    def command(self, name_or_opcode: str | int) -> CommandDef:
        if isinstance(name_or_opcode, int):
            return self.commands_by_opcode[name_or_opcode]
        return self.commands_by_name[name_or_opcode]

    def telemetry(self, name_or_id: str | int) -> TelemetryDef:
        if isinstance(name_or_id, int):
            return self.telemetry_by_id[name_or_id]
        return self.telemetry_by_name[name_or_id]


def _opt_float(val: Any) -> float | None:
    return None if val is None else float(val)
