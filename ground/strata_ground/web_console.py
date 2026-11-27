"""Zero-Dependency Live Web Mission Control Server for StrataGround."""

from __future__ import annotations

import http.server
import json
import pathlib
import socketserver
import threading
from typing import Any

from .cmd_seq_table import SequenceCompiler
from .dictionary import MissionDictionary
from .stream_server import TelemetryBroker
from .test_api import GroundTestSession

WEB_ROOT = pathlib.Path(__file__).resolve().parents[1] / "web"


class _ThreadedHttpServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class GroundWebConsole:
    """Serves the StrataGround browser console and JSON REST API over standard Python http.server."""

    def __init__(
        self,
        dictionary: MissionDictionary,
        broker: TelemetryBroker,
        session: GroundTestSession,
        host: str = "127.0.0.1",
        port: int = 8088,
    ) -> None:
        self.dict = dictionary
        self.broker = broker
        self.session = session
        self.seq_compiler = SequenceCompiler(dictionary)
        self.host = host
        self.port = port
        self._server: _ThreadedHttpServer | None = None
        self._thread: threading.Thread | None = None

    def start(self) -> int:
        console = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
                return  # Keep test output clean

            def _send_json(self, payload: Any, status: int = 200) -> None:
                body = json.dumps(payload).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self) -> None:  # noqa: N802
                if self.path in ("/", "/index.html"):
                    html = (WEB_ROOT / "index.html").read_bytes()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.send_header("Content-Length", str(len(html)))
                    self.end_headers()
                    self.wfile.write(html)
                    return

                if self.path == "/api/dictionary":
                    self._send_json(console.dict.raw)
                    return

                if self.path == "/api/telemetry":
                    rows = [
                        {
                            "channel": s.channel,
                            "channel_id": f"0x{s.channel_id:04X}",
                            "raw_value": s.raw_value,
                            "eng_value": s.eng_value,
                            "unit": s.unit,
                            "limit_state": s.limit_state.value,
                            "timestamp_ns": s.timestamp_ns,
                        }
                        for s in console.broker.latest_tlm.values()
                    ]
                    self._send_json({"telemetry": rows, "all_nominal": console.broker.limits.all_nominal()})
                    return

                if self.path == "/api/events":
                    self._send_json(
                        {
                            "events": console.broker.evr_history[-100:],
                            "pus_tm": [
                                {k: v for k, v in tm.items() if k != "app_data"}
                                for tm in console.broker.pus_tm_history[-100:]
                            ],
                        }
                    )
                    return

                if self.path == "/api/history":
                    self._send_json({"commands": console.broker.cmd_history[-100:]})
                    return

                if self.path == "/api/limits":
                    alerts = [
                        {
                            "channel": a.channel,
                            "state": a.state.value,
                            "previous_state": a.previous_state.value,
                            "value": a.value,
                            "unit": a.unit,
                            "timestamp_ns": a.timestamp_ns,
                        }
                        for a in console.broker.limits.alert_history[-100:]
                    ]
                    self._send_json({"all_nominal": console.broker.limits.all_nominal(), "alerts": alerts})
                    return

                self._send_json({"error": "Not found"}, status=404)

            def do_POST(self) -> None:  # noqa: N802
                length = int(self.headers.get("Content-Length", "0"))
                raw_body = self.rfile.read(length) if length > 0 else b"{}"
                try:
                    req = json.loads(raw_body.decode("utf-8"))
                except json.JSONDecodeError as exc:
                    self._send_json({"error": str(exc)}, status=400)
                    return

                if self.path == "/api/command":
                    try:
                        if req.get("mode") == "pus":
                            pkt = console.session.send_pus_tc(
                                int(req["service_type"]),
                                int(req["service_subtype"]),
                                bytes.fromhex(req.get("app_hex", "")),
                            )
                            self._send_json({"status": "sent", "hex": pkt.hex().upper()})
                        else:
                            pkt = console.session.send_cmd(
                                str(req["command"]),
                                bytes.fromhex(req.get("arg_hex", "")),
                            )
                            self._send_json({"status": "sent", "hex": pkt.hex().upper()})
                    except Exception as exc:  # noqa: BLE001
                        self._send_json({"error": str(exc)}, status=400)
                    return

                if self.path == "/api/sequence/compile":
                    try:
                        blob = console.seq_compiler.compile_text(str(req.get("text", "")))
                        steps = console.seq_compiler.decompile(blob)
                        self._send_json(
                            {
                                "bytes": len(blob),
                                "hex": blob.hex().upper(),
                                "steps": [
                                    {
                                        "delay_ms": st.delay_ms,
                                        "relative": st.is_relative,
                                        "opcode": f"0x{st.opcode:04X}",
                                        "command": st.command_name,
                                        "arg": st.arg_u64,
                                    }
                                    for st in steps
                                ],
                            }
                        )
                    except Exception as exc:  # noqa: BLE001
                        self._send_json({"error": str(exc)}, status=400)
                    return

                self._send_json({"error": "Not found"}, status=404)

        self._server = _ThreadedHttpServer((self.host, self.port), Handler)
        actual_port = int(self._server.server_address[1])
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return actual_port

    def stop(self) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            self._server = None
        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None
