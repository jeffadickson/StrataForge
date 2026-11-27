using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace StrataForge.Ground;

/// <summary>
/// Concurrent, zero-external-dependency .NET 11 HTTP Mission Control Web Console and JSON REST API server.
/// </summary>
/// <remarks>
/// <para>
/// Implements <c>STRATA-GDS-005</c>. Serves <c>ground/web/index.html</c> and dispatches concurrent HTTP requests
/// across thread-pool workers using <see cref="HttpListener"/>:
/// </para>
/// <list type="bullet">
///   <item><description><c>GET /</c> — Live Mission Control browser console.</description></item>
///   <item><description><c>GET /api/dictionary</c> — Single-source-of-truth <c>mission_dict.json</c>.</description></item>
///   <item><description><c>GET /api/telemetry</c> — Live calibrated channels and limit badges.</description></item>
///   <item><description><c>GET /api/events</c> — Decoded <c>EventRecord</c> and PUS-C TM stream.</description></item>
///   <item><description><c>GET /api/history</c> — Uplink command audit log.</description></item>
///   <item><description><c>GET /api/limits</c> — Red/Yellow persistence alert history.</description></item>
///   <item><description><c>POST /api/command</c> — Interactive CCSDS TC or PUS-C uplink.</description></item>
///   <item><description><c>POST /api/sequence/compile</c> — Live <c>.seq</c> compiler &amp; round-trip decompiler.</description></item>
/// </list>
/// </remarks>
public sealed class GroundWebConsole : IAsyncDisposable
{
    private readonly MissionDictionary _dictionary;
    private readonly TelemetryBroker _broker;
    private readonly GroundTestSession _session;
    private readonly SequenceCompiler _seqCompiler;
    private readonly string _webRoot;
    private readonly string _host;
    private readonly int _requestedPort;

    private HttpListener? _listener;
    private CancellationTokenSource? _cts;
    private Task? _acceptLoopTask;

    /// <summary>
    /// Gets the actual bound TCP port after <see cref="Start"/> succeeds.
    /// </summary>
    public int BoundPort { get; private set; }

    /// <summary>
    /// Initializes a new <see cref="GroundWebConsole"/> server.
    /// </summary>
    /// <param name="dictionary">Mission dictionary exposed via <c>/api/dictionary</c>.</param>
    /// <param name="broker">Live <see cref="TelemetryBroker"/> supplying telemetry, events, and limits.</param>
    /// <param name="session">Active <see cref="GroundTestSession"/> executing uplink commands.</param>
    /// <param name="webRoot">Directory containing static <c>index.html</c>.</param>
    /// <param name="host">Bind host address (default <c>127.0.0.1</c>).</param>
    /// <param name="port">TCP port (<c>0</c> selects an available ephemeral port).</param>
    public GroundWebConsole(
        MissionDictionary dictionary,
        TelemetryBroker broker,
        GroundTestSession session,
        string webRoot,
        string host = "127.0.0.1",
        int port = 8088)
    {
        ArgumentNullException.ThrowIfNull(dictionary);
        ArgumentNullException.ThrowIfNull(broker);
        ArgumentNullException.ThrowIfNull(session);
        ArgumentException.ThrowIfNullOrWhiteSpace(webRoot);
        _dictionary = dictionary;
        _broker = broker;
        _session = session;
        _seqCompiler = new SequenceCompiler(dictionary);
        _webRoot = webRoot;
        _host = host;
        _requestedPort = port;
    }

    /// <summary>
    /// Starts the concurrent HTTP accept loop and returns the bound TCP port.
    /// </summary>
    /// <returns>The bound TCP port number.</returns>
    public int Start()
    {
        int port = _requestedPort == 0 ? PickFreeTcpPort(_host) : _requestedPort;
        _listener = new HttpListener();
        _listener.Prefixes.Add($"http://{_host}:{port}/");
        _listener.Start();
        BoundPort = port;

        _cts = new CancellationTokenSource();
        CancellationToken ct = _cts.Token;

        _acceptLoopTask = Task.Run(async () =>
        {
            while (!ct.IsCancellationRequested && _listener.IsListening)
            {
                try
                {
                    HttpListenerContext ctx = await _listener.GetContextAsync().WaitAsync(ct).ConfigureAwait(false);
                    _ = Task.Run(() => HandleRequestAsync(ctx, ct), CancellationToken.None);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (HttpListenerException)
                {
                    break;
                }
                catch (ObjectDisposedException)
                {
                    break;
                }
            }
        }, CancellationToken.None);

        return BoundPort;
    }

    /// <summary>
    /// Stops the HTTP listener cleanly and awaits completion of the accept loop.
    /// </summary>
    public async Task StopAsync()
    {
        if (_cts is not null)
        {
            await _cts.CancelAsync().ConfigureAwait(false);
        }

        if (_listener is not null)
        {
            try
            {
                _listener.Stop();
                _listener.Close();
            }
            catch (ObjectDisposedException)
            {
                // Already closed
            }

            _listener = null;
        }

        if (_acceptLoopTask is not null)
        {
            await _acceptLoopTask.ConfigureAwait(false);
            _acceptLoopTask = null;
        }

        _cts?.Dispose();
        _cts = null;
    }

    /// <summary>
    /// Asynchronously stops and releases all server resources.
    /// </summary>
    public async ValueTask DisposeAsync() => await StopAsync().ConfigureAwait(false);

    private async Task HandleRequestAsync(HttpListenerContext ctx, CancellationToken ct)
    {
        string path = ctx.Request.Url?.AbsolutePath ?? "/";
        string method = ctx.Request.HttpMethod.ToUpperInvariant();

        try
        {
            if (method == "GET")
            {
                await HandleGetAsync(ctx, path, ct).ConfigureAwait(false);
            }
            else if (method == "POST")
            {
                await HandlePostAsync(ctx, path, ct).ConfigureAwait(false);
            }
            else
            {
                await WriteJsonAsync(ctx.Response, new { error = "Method not allowed" }, 405, ct).ConfigureAwait(false);
            }
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            try
            {
                await WriteJsonAsync(ctx.Response, new { error = ex.Message }, 400, ct).ConfigureAwait(false);
            }
            catch
            {
                // Connection already aborted by client
            }
        }
        finally
        {
            try
            {
                ctx.Response.Close();
            }
            catch
            {
                // Ignore close races
            }
        }
    }

    private async Task HandleGetAsync(HttpListenerContext ctx, string path, CancellationToken ct)
    {
        if (path is "/" or "/index.html")
        {
            string indexPath = Path.Combine(_webRoot, "index.html");
            byte[] html = await File.ReadAllBytesAsync(indexPath, ct).ConfigureAwait(false);
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "text/html; charset=utf-8";
            ctx.Response.ContentLength64 = html.Length;
            await ctx.Response.OutputStream.WriteAsync(html, ct).ConfigureAwait(false);
            return;
        }

        if (path == "/api/dictionary")
        {
            byte[] raw = Encoding.UTF8.GetBytes(_dictionary.RawJson);
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "application/json";
            ctx.Response.ContentLength64 = raw.Length;
            await ctx.Response.OutputStream.WriteAsync(raw, ct).ConfigureAwait(false);
            return;
        }

        if (path == "/api/telemetry")
        {
            var rows = _broker.GetLatestTelemetrySnapshot().Select(s => new
            {
                channel = s.Channel,
                channel_id = $"0x{s.ChannelId:X4}",
                raw_value = s.RawValue,
                eng_value = s.EngValue,
                unit = s.Unit,
                limit_state = s.LimitState.ToWireString(),
                timestamp_ns = s.TimestampNs,
            }).ToList();

            await WriteJsonAsync(ctx.Response, new { telemetry = rows, all_nominal = _broker.Limits.AllNominal() }, 200, ct).ConfigureAwait(false);
            return;
        }

        if (path == "/api/events")
        {
            var events = _broker.GetEventHistory(100).Select(e => new
            {
                first_tai_ns = e.FirstTaiNs,
                last_tai_ns = e.LastTaiNs,
                event_id = e.EventId,
                event_id_hex = e.EventIdHex,
                arg0 = e.Arg0,
                arg1 = e.Arg1,
                repeat_count = e.RepeatCount,
                severity = e.Severity,
                partition_id = e.PartitionId,
            }).ToList();

            var pusTm = _broker.GetPusTmHistory(100).Select(tm => new
            {
                apid = tm.Apid,
                seq_count = tm.SeqCount,
                service_type = tm.ServiceType,
                service_subtype = tm.ServiceSubtype,
                msg_type_counter = tm.MsgTypeCounter,
                destination_id = tm.DestinationId,
                cuc_tai_ns = tm.CucTaiNs,
                app_data_hex = tm.AppDataHex,
            }).ToList();

            await WriteJsonAsync(ctx.Response, new { events, pus_tm = pusTm }, 200, ct).ConfigureAwait(false);
            return;
        }

        if (path == "/api/history")
        {
            var commands = _broker.GetCommandHistory(100).Select(c => new
            {
                timestamp_ns = c.TimestampNs,
                command = c.Command,
                hex = c.Hex,
            }).ToList();

            await WriteJsonAsync(ctx.Response, new { commands }, 200, ct).ConfigureAwait(false);
            return;
        }

        if (path == "/api/limits")
        {
            var alerts = _broker.Limits.GetAlertHistory(100).Select(a => new
            {
                channel = a.Channel,
                state = a.State.ToWireString(),
                previous_state = a.PreviousState.ToWireString(),
                value = a.Value,
                unit = a.Unit,
                timestamp_ns = a.TimestampNs,
            }).ToList();

            await WriteJsonAsync(ctx.Response, new { all_nominal = _broker.Limits.AllNominal(), alerts }, 200, ct).ConfigureAwait(false);
            return;
        }

        await WriteJsonAsync(ctx.Response, new { error = "Not found" }, 404, ct).ConfigureAwait(false);
    }

    private async Task HandlePostAsync(HttpListenerContext ctx, string path, CancellationToken ct)
    {
        using StreamReader reader = new(ctx.Request.InputStream, Encoding.UTF8);
        string bodyText = await reader.ReadToEndAsync(ct).ConfigureAwait(false);
        if (string.IsNullOrWhiteSpace(bodyText))
        {
            bodyText = "{}";
        }

        using JsonDocument doc = JsonDocument.Parse(bodyText);
        JsonElement root = doc.RootElement;

        if (path == "/api/command")
        {
            string mode = root.TryGetProperty("mode", out JsonElement m) ? m.GetString() ?? "ccsds" : "ccsds";
            if (mode == "pus")
            {
                byte svc = (byte)root.GetProperty("service_type").GetInt32();
                byte sub = (byte)root.GetProperty("service_subtype").GetInt32();
                string appHex = root.TryGetProperty("app_hex", out JsonElement ah) ? ah.GetString() ?? string.Empty : string.Empty;
                byte[] pkt = _session.SendPusTc(svc, sub, Convert.FromHexString(appHex));
                await WriteJsonAsync(ctx.Response, new { status = "sent", hex = Convert.ToHexString(pkt) }, 200, ct).ConfigureAwait(false);
            }
            else
            {
                string cmdName = root.GetProperty("command").GetString() ?? string.Empty;
                string argHex = root.TryGetProperty("arg_hex", out JsonElement ah) ? ah.GetString() ?? string.Empty : string.Empty;
                byte[] pkt = _session.SendCommand(cmdName, Convert.FromHexString(argHex));
                await WriteJsonAsync(ctx.Response, new { status = "sent", hex = Convert.ToHexString(pkt) }, 200, ct).ConfigureAwait(false);
            }

            return;
        }

        if (path == "/api/sequence/compile")
        {
            string seqText = root.TryGetProperty("text", out JsonElement t) ? t.GetString() ?? string.Empty : string.Empty;
            byte[] blob = _seqCompiler.CompileText(seqText);
            IReadOnlyList<SequenceStep> steps = _seqCompiler.Decompile(blob);
            var stepRows = steps.Select(st => new
            {
                delay_ms = st.DelayMs,
                relative = st.IsRelative,
                opcode = $"0x{st.Opcode:X4}",
                command = st.CommandName,
                arg = st.ArgU64,
            }).ToList();

            await WriteJsonAsync(
                ctx.Response,
                new { bytes = blob.Length, hex = Convert.ToHexString(blob), steps = stepRows },
                200,
                ct).ConfigureAwait(false);
            return;
        }

        await WriteJsonAsync(ctx.Response, new { error = "Not found" }, 404, ct).ConfigureAwait(false);
    }

    private static async Task WriteJsonAsync(HttpListenerResponse response, object payload, int statusCode, CancellationToken ct)
    {
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(payload);
        response.StatusCode = statusCode;
        response.ContentType = "application/json";
        response.ContentLength64 = bytes.Length;
        await response.OutputStream.WriteAsync(bytes, ct).ConfigureAwait(false);
    }

    private static int PickFreeTcpPort(string host)
    {
        using TcpListener probe = new(IPAddress.Parse(host), 0);
        probe.Start();
        int port = ((IPEndPoint)probe.LocalEndpoint).Port;
        probe.Stop();
        return port;
    }
}
