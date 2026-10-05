using System.Diagnostics;
using System.Text;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using Xunit;
using static Bromelia.Tests.Fixtures;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters.Tests;

/// <summary>An HttpClient that records the request and gives a fixed answer (a status and body, or no answer).</summary>
internal sealed class FakeHttp : IHttpClient
{
    public HttpRequestSpec? Sent;
    public int Status = 200;
    public byte[] Body = Array.Empty<byte>();
    public string? FailReason;

    public Task<HttpResponse> Send(HttpRequestSpec request, CancellationToken cancel)
    {
        Sent = request;
        if (FailReason is { } r) throw new BroFailure(new BroError("http.failed", ("reason", JsonValue.Of(r))));
        return Task.FromResult(new HttpResponse(Status, new List<KeyValuePair<string, string>>(), Body));
    }
}

/// <summary>Finds the tools it is given.</summary>
internal sealed class MapLocator : IToolLocator
{
    readonly Dictionary<ToolKind, string> _paths;
    public MapLocator(Dictionary<ToolKind, string> paths) { _paths = paths; }
    public ToolInfo Locate(ToolKind tool) => _paths.TryGetValue(tool, out var p) ? new ToolInfo(tool, p, null, new List<string>())
        : new ToolInfo(tool, null, null, new List<string>(), new BroMessage(MessageCode.ToolMissing, Severity.Warning, ("tool", JsonValue.Of(EnumWire.Name(tool)))));
}

/// <summary>http-client.cases.json, notification-sender.cases.json and beta-key-source.cases.json.</summary>
public sealed class HttpAdaptersTests
{
    static void SameError(JsonValue want, BroFailure f)
    {
        Assert.Equal(want["code"]!.AsString, f.Error.Code);
        foreach (var m in want["params"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            Assert.Equal(m.Value, f.Error.Params[m.Key]);
    }

    [Fact]
    public void TheHttpClientCasesPass()
    {
        using var server = new TestHttpServer();
        var closed = TestHttpServer.Closed();
        RunCases("adapters/http-client.cases.json", (id, given, expect) =>
        {
            var url = given["url"]!.AsString!.Replace("<server>", server.Url).Replace("<closed>", closed);
            var headers = (given["headers"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                .Select(m => new KeyValuePair<string, string>(m.Key, m.Value.AsString!)).ToList();
            var request = new HttpRequestSpec(given["method"]!.AsString!, url, headers, given["body"]?.AsString is { } b ? Encoding.UTF8.GetBytes(b) : null);
            var cancelSource = new CancellationSource();
            var cancel = cancelSource.Token;
            if (given["cancelAfter"]?.AsNumber is { } after) _ = Task.Delay(TimeSpan.FromSeconds(after)).ContinueWith(_ => cancelSource.Cancel());
            var watch = Stopwatch.StartNew();
            try
            {
                var response = new PlatformHttpClient().Send(request, cancel).GetAwaiter().GetResult();
                Assert.Null(expect["error"]);
                Assert.Equal(expect["status"]!.AsInteger, response.Status);
                if (expect["json"] is { } json) Assert.Equal(json, JsonValue.Parse(response.Body));
                if (expect["text"]?.AsString is { } text) Assert.Equal(text, Encoding.UTF8.GetString(response.Body));
            }
            catch (BroFailure f)
            {
                Assert.Equal(expect["error"]?.AsString, f.Error.Code);
            }
            if (expect["within"]?.AsNumber is { } within) Assert.True(watch.Elapsed.TotalSeconds < within, watch.Elapsed.ToString());
            return true;
        });
    }

    [Fact]
    public void TheNotificationCasesPass()
    {
        RunCases("adapters/notification-sender.cases.json", (id, given, expect) =>
        {
            var http = new FakeHttp();
            if (given["answer"] is { } answer)
            {
                http.Status = (int)(answer["status"]?.AsInteger ?? 200);
                http.FailReason = answer["reason"]?.AsString;
            }
            var launcher = new ScriptedProcessLauncher { ExitCode = (int)(given["appriseExit"]?.AsInteger ?? 0) };
            var tools = new Dictionary<ToolKind, string>();
            if (given["apprise"] is not { IsNull: true }) tools[ToolKind.Apprise] = "/opt/apprise";
            var sender = new NotificationSender(http, new AppriseTool(launcher, new MapLocator(tools)));
            try
            {
                sender.Send(given["url"]!.AsString!, "T", "B", EnumWire.Parse<StatusWord>(given["status"]!.AsString)!.Value, new CancellationSource().Token)
                    .GetAwaiter().GetResult();
                Assert.Null(expect["error"]);
            }
            catch (BroFailure f)
            {
                Assert.NotNull(expect["error"]);
                SameError(expect["error"]!, f);
            }
            if (expect["request"] is { } want)
            {
                Assert.Equal(want["method"]!.AsString, http.Sent!.Method);
                Assert.Equal(want["url"]!.AsString, http.Sent.Url);
                if (want["json"] is { } json) Assert.Equal(json, JsonValue.Parse(http.Sent.Body!));
                if (want["text"]?.AsString is { } text) Assert.Equal(text, Encoding.UTF8.GetString(http.Sent.Body!));
            }
            if (expect["apprise"] is { } argv)
            {
                var spec = launcher.Started.Single();
                Assert.Equal(argv.AsArray!.Select(a => a.AsString), new[] { spec.Executable }.Concat(spec.Arguments));
                Assert.Equal(expect["appriseEnvironment"]!.AsObject!.Select(m => m.Key + "=" + m.Value.AsString),
                    spec.Environment.OrderBy(kv => kv.Key).Select(kv => kv.Key + "=" + kv.Value));
            }
            return true;
        });
    }

    [Fact]
    public void TheBetaKeyCasesPass()
    {
        RunCases("adapters/beta-key-source.cases.json", (id, given, expect) =>
        {
            var answer = given["answer"]!;
            var http = new FakeHttp
            {
                Status = (int)(answer["status"]?.AsInteger ?? 200),
                FailReason = answer["reason"]?.AsString,
                Body = answer["file"]?.AsString is { } file ? File.ReadAllBytes(Path_(file)) : Encoding.UTF8.GetBytes(answer["text"]?.AsString ?? ""),
            };
            try
            {
                var key = new BetaKeySource(http).CurrentKey(new CancellationSource().Token).GetAwaiter().GetResult();
                Assert.StartsWith(expect["keyPrefix"]!.AsString!, key);
                Assert.Equal(BetaKeyPage.Url, http.Sent!.Url);
                Assert.Equal("GET", http.Sent.Method);
            }
            catch (BroFailure f)
            {
                SameError(expect["error"]!, f);
            }
            return true;
        });
    }
}
