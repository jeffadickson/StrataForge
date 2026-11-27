"""StrataGround — Native Ground Data System (GDS) & Integration/Test (I&T) Suite for STRATA-20.

Driven directly by `strata-gen`'s single-source-of-truth `mission_dict.json` and `MANIFEST_HASH`:
- Unified telemetry/command/event dictionary with engineering-unit calibration & red/yellow limits
- CCSDS Space Packet, TC Transfer Frame (CRC-16 FECF), and ECSS-E-ST-70-41C (PUS-C) codecs
- Flight Sequence (.seq) and Parameter Table (ParamBank) binary compilers & decompilers
- Pluggable Telemetry/Command Stream Broker with Handler pipeline, standard PCAP capture & CSV export
- Automated I&T Test Scripting API (`GroundTestSession`) for FlatSat / TVAC / Verilator HIL
- Zero-dependency Web Mission Control Console (`web_console.py`)
"""

from .cmd_seq_table import (
    ParamTableCompiler,
    SequenceCompiler,
    crc16_ccitt,
    crc32c,
    decode_event_record,
    decode_nv_log_record,
    decode_pus_c_tm,
    encode_pus_c_tc,
    encode_tc_frame,
)
from .dictionary import CommandDef, MissionDictionary, TelemetryDef
from .limits import LimitAlert, LimitMonitor, LimitSpec, LimitState
from .stream_server import PcapReader, PcapWriter, TelemetryBroker, export_tlm_csv
from .test_api import GroundTestSession

__all__ = [
    "CommandDef",
    "GroundTestSession",
    "LimitAlert",
    "LimitMonitor",
    "LimitSpec",
    "LimitState",
    "MissionDictionary",
    "ParamTableCompiler",
    "PcapReader",
    "PcapWriter",
    "SequenceCompiler",
    "TelemetryBroker",
    "TelemetryDef",
    "crc16_ccitt",
    "crc32c",
    "decode_event_record",
    "decode_nv_log_record",
    "decode_pus_c_tm",
    "encode_pus_c_tc",
    "encode_tc_frame",
    "export_tlm_csv",
]
