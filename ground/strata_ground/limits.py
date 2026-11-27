"""Real-time Red/Yellow Telemetry Limits Monitor with Persistence Debouncing."""

from __future__ import annotations

import dataclasses
import enum
from typing import Callable

from .dictionary import MissionDictionary


class LimitState(str, enum.Enum):
    NOMINAL = "NOMINAL"
    WARN_LO = "WARN_LO"
    WARN_HI = "WARN_HI"
    ERROR_LO = "ERROR_LO"
    ERROR_HI = "ERROR_HI"


@dataclasses.dataclass(frozen=True)
class LimitSpec:
    channel: str
    lo_err: float | None = None
    lo_warn: float | None = None
    hi_warn: float | None = None
    hi_err: float | None = None
    persistence: int = 1  # Consecutive out-of-limit samples before alerting


@dataclasses.dataclass(frozen=True)
class LimitAlert:
    channel: str
    state: LimitState
    previous_state: LimitState
    value: float
    unit: str
    timestamp_ns: int


class LimitMonitor:
    """Evaluates calibrated engineering telemetry against Red/Yellow bounds with persistence."""

    def __init__(self, dictionary: MissionDictionary | None = None) -> None:
        self._specs: dict[str, LimitSpec] = {}
        self._units: dict[str, str] = {}
        self._states: dict[str, LimitState] = {}
        self._pending_state: dict[str, LimitState] = {}
        self._pending_count: dict[str, int] = {}
        self._callbacks: list[Callable[[LimitAlert], None]] = []
        self.alert_history: list[LimitAlert] = []

        if dictionary is not None:
            for t in dictionary.telemetry_by_name.values():
                self._units[t.name] = t.unit
                if any(v is not None for v in (t.limit_lo_err, t.limit_lo_warn, t.limit_hi_warn, t.limit_hi_err)):
                    self.register(
                        LimitSpec(
                            channel=t.name,
                            lo_err=t.limit_lo_err,
                            lo_warn=t.limit_lo_warn,
                            hi_warn=t.limit_hi_warn,
                            hi_err=t.limit_hi_err,
                        )
                    )

    def register(self, spec: LimitSpec, unit: str = "") -> None:
        self._specs[spec.channel] = spec
        if unit:
            self._units[spec.channel] = unit
        self._states.setdefault(spec.channel, LimitState.NOMINAL)

    def on_alert(self, callback: Callable[[LimitAlert], None]) -> None:
        self._callbacks.append(callback)

    def state(self, channel: str) -> LimitState:
        return self._states.get(channel, LimitState.NOMINAL)

    def all_nominal(self) -> bool:
        return all(st == LimitState.NOMINAL for st in self._states.values())

    def evaluate(self, channel: str, eng_value: float, timestamp_ns: int = 0) -> LimitState:
        spec = self._specs.get(channel)
        if spec is None:
            return LimitState.NOMINAL

        raw_state = _classify(spec, eng_value)
        cur_state = self._states.get(channel, LimitState.NOMINAL)

        if raw_state == cur_state:
            self._pending_state[channel] = cur_state
            self._pending_count[channel] = 0
            return cur_state

        if self._pending_state.get(channel) == raw_state:
            self._pending_count[channel] = self._pending_count.get(channel, 0) + 1
        else:
            self._pending_state[channel] = raw_state
            self._pending_count[channel] = 1

        if self._pending_count[channel] >= max(1, spec.persistence):
            self._states[channel] = raw_state
            self._pending_count[channel] = 0
            alert = LimitAlert(
                channel=channel,
                state=raw_state,
                previous_state=cur_state,
                value=eng_value,
                unit=self._units.get(channel, ""),
                timestamp_ns=timestamp_ns,
            )
            self.alert_history.append(alert)
            for cb in self._callbacks:
                cb(alert)
            return raw_state

        return cur_state


def _classify(spec: LimitSpec, v: float) -> LimitState:
    if spec.lo_err is not None and v < spec.lo_err:
        return LimitState.ERROR_LO
    if spec.hi_err is not None and v > spec.hi_err:
        return LimitState.ERROR_HI
    if spec.lo_warn is not None and v < spec.lo_warn:
        return LimitState.WARN_LO
    if spec.hi_warn is not None and v > spec.hi_warn:
        return LimitState.WARN_HI
    return LimitState.NOMINAL
