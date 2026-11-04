using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using Bromelia.Adapters;
using Bromelia.Domain;
using Bromelia.Engine;
using Bromelia.Foundation;
using Bromeliad;
using Xunit;
using Xunit.Abstractions;
using Xunit.Sdk;

namespace Bromelia.Hosts.Tests;

/// <summary>The realDisc shared scenarios (decisions 302–304) through the real root: real makemkvcon and mkvmerge, real disk,
/// the image only read. Every scenario whose phase is built is a test case: skipped with the reason when its variable isn't
/// set, and a failure is a failure when it is. Nothing of the user's is read: a scratch world, an empty home, PATH alone from
/// the environment, no system keystore, no drives. Exactly what the phase 3 real-disc scenarios use is supported; anything
/// else fails as "not supported in a real world: &lt;key&gt;", so a later one can't pass half-read.</summary>
public sealed class RealDiscScenarioTests
{
    readonly ITestOutputHelper _output;

    public RealDiscScenarioTests(ITestOutputHelper output) { _output = output; }

    [RealDiscScenarios]
    public async Task PlaysARealDiscScenario(string id)
    {
        var run = new RealDiscRun(id, RealDiscCatalog.Load(id), _output.WriteLine);
        await run.Play();
    }

    /// <summary>A later realDisc scenario can't pass half-read: the phase 5 ones fail before anything runs.</summary>
    [Theory]
    [InlineData("realdisc-custom-tracks-rename-script", "realdisc-custom-tracks-rename-script step 0 (start): not supported in a real world: given.configPatch")]
    [InlineData("realdisc-dvd-navigation-from-iso", "realdisc-dvd-navigation-from-iso step 1 (open): not supported in a real world: open")]
    public void WhatAPhase3RunDoesntKnowFails(string id, string failure)
    {
        var run = new RealDiscRun(id, RealDiscCatalog.Load(id), _output.WriteLine);
        Assert.Equal(failure, Assert.Throws<RealDiscFailure>(run.CheckSupported).Message);
    }
}

/// <summary>Each selected scenario as its own case, a skipped one with its reason (xunit 2 has no skip at run time).</summary>
[XunitTestCaseDiscoverer("Bromelia.Hosts.Tests.RealDiscDiscoverer", "Bromelia.Hosts.Tests")]
[AttributeUsage(AttributeTargets.Method)]
public sealed class RealDiscScenariosAttribute : FactAttribute { }

public sealed class RealDiscDiscoverer : IXunitTestCaseDiscoverer
{
    readonly IMessageSink _sink;

    public RealDiscDiscoverer(IMessageSink sink) { _sink = sink; }

    public IEnumerable<IXunitTestCase> Discover(ITestFrameworkDiscoveryOptions options, ITestMethod method, IAttributeInfo attribute)
    {
        var display = options.MethodDisplayOrDefault();
        var displayOptions = options.MethodDisplayOptionsOrDefault();
        foreach (var id in RealDiscCatalog.Selected())
            yield return RealDiscCatalog.Image(id) is null
                ? new XunitSkippedDataRowTestCase(_sink, display, displayOptions, method, RealDiscCatalog.SkipReason(id), new object[] { id })
                : new XunitTestCase(_sink, display, displayOptions, method, new object[] { id });
    }
}

/// <summary>shared/scenarios/json: the scenarios with realDisc whose phase is built.</summary>
public static class RealDiscCatalog
{
    const int BuiltPhase = 3;

    public static readonly string Shared = FindShared();

    static string FindShared([System.Runtime.CompilerServices.CallerFilePath] string source = "")
    {
        foreach (var start in new[] { Path.GetDirectoryName(source), AppContext.BaseDirectory })
            for (var dir = start is null ? null : new DirectoryInfo(start); dir != null; dir = dir.Parent)
            {
                var candidate = Path.Combine(dir.FullName, "shared");
                if (Directory.Exists(Path.Combine(candidate, "scenarios", "json"))) return candidate;
            }
        throw new InvalidOperationException("shared/scenarios/json not found above " + source + " or " + AppContext.BaseDirectory);
    }

    public static JsonValue Read(params string[] path) =>
        JsonValue.Parse(File.ReadAllBytes(Path.Combine(new[] { Shared }.Concat(path).ToArray()))) ?? throw new InvalidOperationException(path[^1] + " isn't JSON");

    public static JsonValue Load(string id) => Read("scenarios", "json", id + ".json");

    public static IReadOnlyList<string> Selected() => Directory.GetFiles(Path.Combine(Shared, "scenarios", "json"), "*.json")
        .Select(f => Path.GetFileNameWithoutExtension(f)!)
        .Where(id => Load(id) is var s && s["realDisc"] != null && s["phase"]?.AsInteger is { } phase && phase <= BuiltPhase)
        .OrderBy(id => id, StringComparer.Ordinal)
        .ToList();

    static string Variable(string id) => Load(id)["realDisc"]!["env"]!.AsString!;

    /// <summary>The image the scenario's variable names; null when it isn't set.</summary>
    public static string? Image(string id) => Environment.GetEnvironmentVariable(Variable(id)) is { Length: > 0 } path ? path : null;

    /// <summary>Why it doesn't run here, with the room it would have (realdisc-one-pass writes 13.8 GB).</summary>
    public static string SkipReason(string id)
    {
        var temp = Path.GetTempPath();
        var free = new DriveInfo(Path.GetPathRoot(temp)!).AvailableFreeSpace / 1e9;
        return $"{id}: {Variable(id)} isn't set ({free.ToString("0.0", CultureInfo.InvariantCulture)} GB free in {temp})";
    }
}

/// <summary>A step of a real run didn't hold: what was expected, and what was found.</summary>
public sealed class RealDiscFailure : Exception
{
    public RealDiscFailure(string expected, string found) : base(expected + " / " + found) { }

    public RealDiscFailure(string message) : base(message) { }
}

/// <summary>One scenario through the real root (decision 302): a fresh world under the temporary folder (one volume, so
/// staging → library is a rename), removed afterwards; on a failure the job's logs and transcripts are copied out first.</summary>
public sealed class RealDiscRun
{
    readonly string _id;
    readonly JsonValue _scenario;
    readonly Action<string> _say;
    readonly string _root = Path.Combine(Path.GetTempPath(), "bromelia-realdisc-" + Guid.NewGuid().ToString("N"));
    readonly Dictionary<string, Id> _jobs = new();
    readonly Stopwatch _clock = Stopwatch.StartNew();
    string Data => Path.Combine(_root, "data");
    string Home => Path.Combine(_root, "home");
    string Library => Path.Combine(_root, "library");
    LocalEngine? _engine;
    LocalEngine Engine => _engine!;
    int _step;
    string _kind = "start";

    public RealDiscRun(string id, JsonValue scenario, Action<string> say)
    {
        _id = id;
        _scenario = scenario;
        _say = say;
    }

    public async Task Play()
    {
        CheckSupported();
        try
        {
            Prepare();
            _engine = CompositionRoot.Build(new RootOptions(Data, null, Home, SystemKeystore: false, SystemTools: true, Drives: false,
                new Dictionary<string, string> { ["PATH"] = Environment.GetEnvironmentVariable("PATH") ?? "" }));
            await Engine.Start().WaitAsync(TimeSpan.FromSeconds(60));
            foreach (var step in _scenario["steps"]!.AsArray!)
            {
                _step++;
                var (kind, value) = step.AsObject!.First();
                _kind = kind;
                if (kind == "rip") await Rip(value);
                else await Expect(value);
            }
            await Finish();
        }
        catch (Exception e)
        {
            await KeepLogs();
            if (e is RealDiscFailure) throw new RealDiscFailure($"{_id} step {_step} ({_kind}): {e.Message}");
            throw new RealDiscFailure($"{_id} step {_step} ({_kind}): {(e is BroFailure f ? "error " + f.Error.Code + " " + f.Error.Params : e.ToString())}");
        }
        finally
        {
            await Stop();
            if (Directory.Exists(_root)) Directory.Delete(_root, true);
        }
    }

    // ---- what a real world supports ----

    static void Only(string where, JsonValue value, params string[] keys)
    {
        foreach (var (key, _) in value.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            if (!keys.Contains(key)) throw new RealDiscFailure("not supported in a real world: " + where + key);
    }

    public void CheckSupported()
    {
        try
        {
            Only("", _scenario, "title", "phase", "realDisc", "from", "given", "steps");
            Only("given.", _scenario["given"] ?? JsonValue.Of(), "profile");
            foreach (var step in _scenario["steps"]!.AsArray!)
            {
                _step++;
                var (kind, value) = step.AsObject!.First();
                _kind = kind;
                if (kind == "rip") Only("rip.", value, "iso", "as");
                else if (kind == "expect")
                {
                    Only("expect.", value, "job", "jobs", "log", "calls", "files", "sums");
                    if (value["job"] is { } job) Only("expect.job.", job, "name", "state", "outcome", "error", "noError", "files");
                    if (value["jobs"] is { } jobs) Only("expect.jobs.", jobs, "count", "outcomes", "states", "kinds");
                    if (value["log"] is { } log) Only("expect.log.", log, "job", "include", "exclude");
                    if (value["calls"] is { } calls)
                    {
                        Only("expect.calls.", calls, "makemkvcon");
                        Only("expect.calls.makemkvcon.", calls["makemkvcon"]!, "none", "count", "include", "exclude", "counts");
                    }
                    if (value["files"] is { } files) Only("expect.files.", files, "under", "exactly", "include", "exclude");
                    if (value["sums"] is { } sums) Only("expect.sums.", sums, "folder", "ok", "files");
                }
                else throw new RealDiscFailure("not supported in a real world: " + kind);
            }
            _step = 0;
            _kind = "start";
        }
        catch (RealDiscFailure f)
        {
            throw new RealDiscFailure($"{_id} step {_step} ({_kind}): {f.Message}");
        }
    }

    // ---- the world ----

    void Prepare()
    {
        Directory.CreateDirectory(Data);
        Directory.CreateDirectory(Home);
        Directory.CreateDirectory(Library);
        var marker = LibraryMarker.Make("lib", Library, "Library", new SystemClock().Now());
        File.WriteAllBytes(Path.Combine(Library, LibraryMarker.FileName), LibraryMarker.Encode(marker));
        File.WriteAllBytes(Path.Combine(Data, "config.json"), JsonValue.EncodeCanonical(Configuration()));
    }

    /// <summary>config3-scenario-base.json with the scenario's profile over the default one, {root} filled in.</summary>
    JsonValue Configuration()
    {
        var config = RealDiscCatalog.Read("fixtures", "config", "config3-scenario-base.json");
        if (_scenario["given"]?["profile"] is { } profile)
        {
            var profiles = config["profiles"]!.AsArray!.ToList();
            int at = profiles.FindIndex(p => p["id"]?.AsString == "default");
            profiles[at] = MergePatch(profiles[at], profile);
            config = MergePatch(config, JsonValue.Of(("profiles", JsonValue.Of(profiles))));
        }
        return Fill(config);
    }

    /// <summary>RFC 7386.</summary>
    static JsonValue MergePatch(JsonValue target, JsonValue patch)
    {
        if (patch is not JsonValue.Object p) return patch;
        var members = (target as JsonValue.Object)?.Members.ToList() ?? new List<KeyValuePair<string, JsonValue>>();
        foreach (var (key, value) in p.Members)
        {
            int at = members.FindIndex(m => m.Key == key);
            if (value.IsNull)
            {
                if (at >= 0) members.RemoveAt(at);
                continue;
            }
            var merged = MergePatch(at >= 0 ? members[at].Value : JsonValue.Null.Instance, value);
            if (at >= 0) members[at] = new(key, merged);
            else members.Add(new(key, merged));
        }
        return new JsonValue.Object(members);
    }

    JsonValue Fill(JsonValue v) => v switch
    {
        JsonValue.String s => JsonValue.Of(s.Value.Replace("{root}", _root)),
        JsonValue.Array a => JsonValue.Of(a.Items.Select(Fill)),
        JsonValue.Object o => new JsonValue.Object(o.Members.Select(m => new KeyValuePair<string, JsonValue>(m.Key, Fill(m.Value))).ToList()),
        _ => v,
    };

    string Path_(string relative) => Path.Combine(_root, relative.Replace('/', Path.DirectorySeparatorChar));

    // ---- the steps ----

    async Task Rip(JsonValue v)
    {
        var iso = RealDiscCatalog.Image(_id)!;
        var job = await Engine.StartJob(new JobRequest(JobKind.VideoDisc, Path.GetFileNameWithoutExtension(iso), false,
            Options: JsonValue.Of(Array.Empty<(string, JsonValue)>()), Details: DiscRequest.Encode(new DiscRequest(new DiscSource.Iso(iso), null))));
        _jobs[v["as"]?.AsString ?? "job" + (_jobs.Count + 1)] = job.Id;
    }

    /// <summary>Real time (decision 302): until every job has finished, or the cap, which fails with the state and the step.</summary>
    async Task Settle()
    {
        var cap = TimeSpan.FromMinutes(_id == "realdisc-one-pass" ? 60 : 10);
        var deadline = DateTime.UtcNow + cap;
        foreach (var (name, id) in _jobs)
            while (true)
            {
                var detail = await Engine.Job(id);
                if (detail.Job.State == JobState.Finished) break;
                if (DateTime.UtcNow > deadline)
                    throw new RealDiscFailure($"job {name} to finish within {cap.TotalMinutes} min", $"{EnumWire.Name(detail.Job.State)}, steps "
                        + string.Join(" ", detail.Steps.Select(s => EnumWire.Name(s.Kind) + ":" + EnumWire.Name(s.State))));
                await Task.Delay(1000);
            }
    }

    async Task Expect(JsonValue expect)
    {
        await Settle();
        var named = expect["job"]?["name"]?.AsString;
        foreach (var (key, want) in expect.AsObject!)
            switch (key)
            {
                case "job":
                    await ExpectJob(want);
                    break;
                case "jobs":
                    await ExpectJobs(want);
                    break;
                case "log":
                    await ExpectLog(want, named);
                    break;
                case "calls":
                    ExpectCalls(want["makemkvcon"]!, named);
                    break;
                case "files":
                    ExpectFiles(want);
                    break;
                case "sums":
                    ExpectSums(want);
                    break;
            }
    }

    string Only_(string? named) => named ?? (_jobs.Count == 1 ? _jobs.Keys.First() : throw new RealDiscFailure("one job, or a name", _jobs.Count + " jobs"));

    Id Job(string name) => _jobs.TryGetValue(name, out var id) ? id : throw new RealDiscFailure("a job named " + name, "none");

    async Task ExpectJob(JsonValue want)
    {
        var name = Only_(want["name"]?.AsString);
        var detail = await Engine.Job(Job(name));
        var json = JobDetailDto.Json(detail);
        try
        {
            foreach (var (key, value) in want.AsObject!)
                switch (key)
                {
                    case "state" or "outcome" or "error":
                        Same(value, json[key], $"job {name} {key}");
                        break;
                    case "noError":
                        if (value.AsBool == true && json["error"] != null) throw new RealDiscFailure($"job {name} without an error", Text(json["error"]));
                        break;
                    case "files":
                        Same(value, JsonValue.Of(detail.Files.Count), $"job {name} files");
                        break;
                }
        }
        catch (RealDiscFailure f)
        {
            var log = (await Engine.JobLog(Job(name), 0)).Select(l => MessageCode.Wire(l.Code) + (l.Params.Members.Count > 0 ? " " + Text(l.Params) : ""));
            throw new RealDiscFailure(f.Message + $"\n  (job {name}: {EnumWire.Name(detail.Job.State)}, error {Text(json["error"])}, steps "
                + string.Join(" ", detail.Steps.Select(s => EnumWire.Name(s.Kind) + ":" + EnumWire.Name(s.State))) + ", log:\n    " + string.Join("\n    ", log) + ")");
        }
    }

    async Task ExpectJobs(JsonValue want)
    {
        var page = await Engine.Jobs(new JobQueryDto(Array.Empty<JobState>(), Array.Empty<Outcome>(), Limit: JobQueryDto.MaxLimit));
        if (want["count"]?.AsInteger is { } count) Same(JsonValue.Of(count), JsonValue.Of(page.Items.Count), "jobs count");
        if (want["outcomes"] is { } outcomes)
            Same(outcomes, JsonValue.Of(page.Items.Select(j => j.Outcome is { } o ? JsonValue.Of(EnumWire.Name(o)) : JsonValue.Null.Instance)), "jobs outcomes");
        if (want["states"] is { } states) Same(states, JsonValue.Of(page.Items.Select(j => JsonValue.Of(EnumWire.Name(j.State)))), "jobs states");
        if (want["kinds"] is { } kinds) Same(kinds, JsonValue.Of(page.Items.Select(j => JsonValue.Of(EnumWire.Name(j.Kind)))), "jobs kinds");
    }

    async Task ExpectLog(JsonValue want, string? named)
    {
        var name = Only_(want["job"]?.AsString ?? named);
        var codes = (await Engine.JobLog(Job(name), 0)).Select(l => MessageCode.Wire(l.Code)).ToList();
        foreach (var c in want["include"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (!codes.Contains(c)) throw new RealDiscFailure($"{c} in the log of {name}", Show(codes));
        foreach (var c in want["exclude"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (codes.Contains(c)) throw new RealDiscFailure($"no {c} in the log of {name}", Show(codes));
    }

    /// <summary>The makemkvcon runs of the job (decision 304): each run's "==== … $ &lt;command&gt;" header in its makemkv.txt.</summary>
    List<string> Calls(Id job)
    {
        var path = Path.Combine(Data, "jobs", job.Value, SealStep.Transcript);
        if (!File.Exists(path)) return new List<string>();
        return File.ReadAllLines(path).Where(l => l.StartsWith("==== ", StringComparison.Ordinal) && l.Contains(" $ ", StringComparison.Ordinal))
            .Select(l => l[(l.IndexOf(" $ ", StringComparison.Ordinal) + 3)..]).ToList();
    }

    void ExpectCalls(JsonValue w, string? named)
    {
        var calls = Calls(Job(Only_(named)));
        string Found() => calls.Count + " calls: " + string.Join("\n  ", calls);
        if (w["none"]?.AsBool == true && calls.Count > 0) throw new RealDiscFailure("no makemkvcon call", Found());
        if (w["count"]?.AsInteger is { } count && calls.Count != count) throw new RealDiscFailure($"{count} makemkvcon calls", Found());
        foreach (var s in w["include"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (!calls.Any(c => c.Contains(s, StringComparison.Ordinal))) throw new RealDiscFailure($"a makemkvcon call with \"{s}\"", Found());
        foreach (var s in w["exclude"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (calls.Any(c => c.Contains(s, StringComparison.Ordinal))) throw new RealDiscFailure($"no makemkvcon call with \"{s}\"", Found());
        foreach (var (s, n) in w["counts"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
            if (calls.Count(c => c.Contains(s, StringComparison.Ordinal)) != n.AsInteger)
                throw new RealDiscFailure($"{n.AsInteger} makemkvcon calls with \"{s}\"", Found());
    }

    /// <summary>Every file and folder under the folder, relative to it with '/', folders without a final '/'.</summary>
    static (List<string> Files, List<string> Folders) Walk(string folder)
    {
        var files = new List<string>();
        var folders = new List<string>();
        void Visit(string dir, string rel)
        {
            foreach (var e in new DirectoryInfo(dir).EnumerateFileSystemInfos())
            {
                var r = rel.Length == 0 ? e.Name : rel + "/" + e.Name;
                if (e is DirectoryInfo)
                {
                    folders.Add(r);
                    Visit(e.FullName, r);
                }
                else files.Add(r);
            }
        }
        if (Directory.Exists(folder)) Visit(folder, "");
        return (files, folders);
    }

    static bool Hidden(string path) => path.Split('/').Any(p => p.StartsWith('.'));

    void ExpectFiles(JsonValue want)
    {
        var (files, folders) = Walk(want["under"]?.AsString is { } under ? Path_(under.TrimEnd('/')) : _root);
        var all = files.Concat(folders).ToList();
        string where = want["under"]?.AsString is { } u ? " under " + u : "";
        if (want["exactly"]?.AsArray is { } exactly)
        {
            var listed = files.Where(f => !Hidden(f))
                .Concat(folders.Where(d => !Hidden(d) && !all.Any(x => x.StartsWith(d + "/", StringComparison.Ordinal))).Select(d => d + "/"))
                .OrderBy(x => x, StringComparer.Ordinal).ToList();
            var patterns = exactly.Select(p => p.AsString!).ToList();
            if (!(listed.All(f => patterns.Any(p => Glob(p, f))) && patterns.All(p => listed.Any(f => Glob(p, f)))))
                throw new RealDiscFailure($"exactly {Show(patterns.OrderBy(x => x, StringComparer.Ordinal))}{where}", Show(listed));
        }
        foreach (var p in want["include"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (!all.Any(f => Glob(p, f)))
                throw new RealDiscFailure($"{p}{where}", Show(all.Where(f => !Hidden(f) || p.Contains('.')).OrderBy(x => x, StringComparer.Ordinal)));
        foreach (var p in want["exclude"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (all.Where(f => Glob(p, f)).ToList() is { Count: > 0 } found)
                throw new RealDiscFailure($"no {p}{where}", Show(found));
    }

    /// <summary>A path pattern: * within one component, ** across components; a component starting with '.' is matched only
    /// by a pattern component that writes the dot.</summary>
    static bool Glob(string pattern, string path)
    {
        var parts = path.TrimEnd('/').Split('/');
        var pats = pattern.TrimEnd('/').Split('/');
        return GlobParts(pats, 0, parts, 0) && (!pattern.EndsWith('/') || path.EndsWith('/'));
    }

    static bool GlobParts(string[] pats, int i, string[] parts, int j)
    {
        if (i == pats.Length) return j == parts.Length;
        if (pats[i] == "**")
        {
            for (int k = j; k <= parts.Length; k++)
                if (GlobParts(pats, i + 1, parts, k)) return true;
            return false;
        }
        if (j == parts.Length) return false;
        if (parts[j].StartsWith('.') && !pats[i].StartsWith('.')) return false;
        var regex = "^" + Regex.Escape(pats[i]).Replace("\\*", "[^/]*").Replace("\\?", "[^/]") + "$";
        return Regex.IsMatch(parts[j], regex) && GlobParts(pats, i + 1, parts, j + 1);
    }

    /// <summary>What's wrong with the folder's SHA256SUMS, every file it lists hashed again from disk, and what it lists.</summary>
    static (List<string> Bad, List<SumEntry> Entries) CheckSums(string folder)
    {
        var entries = Sha256Sums.Parse(File.ReadAllText(Path.Combine(folder, Sha256Sums.FileName), Encoding.UTF8)).ToList();
        var bad = new List<string>();
        foreach (var e in entries)
        {
            var path = Path.Combine(folder, e.Path.Replace('/', Path.DirectorySeparatorChar));
            if (!File.Exists(path))
            {
                bad.Add(e.Path + " missing");
                continue;
            }
            var hasher = new Sha256Hasher();
            using var stream = File.OpenRead(path);
            var chunk = new byte[1 << 22];
            for (int n; (n = stream.Read(chunk, 0, chunk.Length)) > 0;) hasher.Update(n == chunk.Length ? chunk : chunk[..n]);
            if (hasher.Finish() != e.Sha256) bad.Add(e.Path + " differs");
        }
        return (bad, entries);
    }

    void ExpectSums(JsonValue want)
    {
        var rel = want["folder"]!.AsString!;
        if (!File.Exists(Path.Combine(Path_(rel), Sha256Sums.FileName))) throw new RealDiscFailure($"{rel}/{Sha256Sums.FileName}", "none");
        var (bad, entries) = CheckSums(Path_(rel));
        if (want["ok"]?.AsBool is { } ok && (bad.Count == 0) != ok)
            throw new RealDiscFailure($"sha256sum -c in {rel} {(ok ? "passing" : "failing")}", bad.Count == 0 ? "it passes" : string.Join(", ", bad));
        if (want["files"] is { } files)
            Same(JsonValue.Of(files.AsArray!.Select(f => f.AsString!).OrderBy(x => x, StringComparer.Ordinal).Select(x => JsonValue.Of(x))),
                JsonValue.Of(entries.Select(e => e.Path).OrderBy(x => x, StringComparer.Ordinal).Select(x => JsonValue.Of(x))), $"the files {rel}/SHA256SUMS lists");
    }

    static string Show(IEnumerable<string> items) => "[" + string.Join(", ", items.Select(i => "\"" + i + "\"")) + "]";

    static string Text(JsonValue? v) => v == null ? "null" : Regex.Replace(v.ToString(), @"\s*\n\s*", " ").Trim();

    static void Same(JsonValue want, JsonValue? got, string what)
    {
        if (!Matches(want, got)) throw new RealDiscFailure($"{what} {Text(want)}", Text(got));
    }

    /// <summary>Objects by every wanted member, arrays element by element, numbers by value.</summary>
    static bool Matches(JsonValue want, JsonValue? got) => want switch
    {
        JsonValue.Null => got is null or JsonValue.Null,
        JsonValue.Object o => got is JsonValue.Object && o.Members.All(m => Matches(m.Value, got[m.Key])),
        JsonValue.Array a => got is JsonValue.Array ga && ga.Items.Count == a.Items.Count && a.Items.Zip(ga.Items).All(p => Matches(p.First, p.Second)),
        JsonValue.Integer or JsonValue.Number => got?.AsNumber is { } n && want.AsNumber == n,
        _ => Equals(want, got),
    };

    // ---- the end ----

    /// <summary>What every real run must leave true, whatever its scenario says: no makemkvcon run named a drive, nothing was
    /// written to the empty home, every SHA256SUMS in the library holds from disk. Then the run's line for the report.</summary>
    async Task Finish()
    {
        _step++;
        _kind = "end";
        foreach (var (name, id) in _jobs)
            foreach (var call in Calls(id))
                if (call.Contains(" disc:", StringComparison.Ordinal) || call.Contains(" dev:", StringComparison.Ordinal))
                    throw new RealDiscFailure($"job {name} reading only the image", call);
        if (Directory.EnumerateFileSystemEntries(Home).Any()) throw new RealDiscFailure("the home left empty", Show(Walk(Home).Files));
        if ((await Engine.Drives()).Count != 0) throw new RealDiscFailure("no drives", "some");
        var (files, _) = Walk(Library);
        var sums = files.Where(f => f.EndsWith("/" + Sha256Sums.FileName, StringComparison.Ordinal)).ToList();
        foreach (var s in sums)
        {
            var (bad, _) = CheckSums(Path_("library/" + s[..s.LastIndexOf('/')]));
            if (bad.Count > 0) throw new RealDiscFailure("sha256sum -c passing in " + s, string.Join(", ", bad));
        }
        long bytes = files.Sum(f => new FileInfo(Path_("library/" + f)).Length);
        _say($"{_id}: {_clock.Elapsed.TotalSeconds:0} s, {files.Count(f => !Hidden(f))} files in the library ({bytes} bytes), {sums.Count} SHA256SUMS hold: "
            + Show(files.Where(f => !Hidden(f)).OrderBy(x => x, StringComparer.Ordinal)));
    }

    async Task Stop()
    {
        if (_engine is null) return;
        try
        {
            await Engine.Stop().WaitAsync(TimeSpan.FromSeconds(60));
        }
        catch (Exception e)
        {
            _say($"{_id}: stopping the engine: {e.Message}");
        }
    }

    /// <summary>The engine log and each job folder's own files (its logs and transcripts; never its home), to
    /// BROMELIA_TEST_OUTPUT or the temporary folder.</summary>
    Task KeepLogs()
    {
        var output = Path.Combine(Environment.GetEnvironmentVariable("BROMELIA_TEST_OUTPUT") is { Length: > 0 } o ? o : Path.GetTempPath(), "bromelia-realdisc-failed", _id);
        Directory.CreateDirectory(output);
        var engineLog = Path.Combine(Data, "logs", "engine.log");
        if (File.Exists(engineLog)) File.Copy(engineLog, Path.Combine(output, "engine.log"), true);
        foreach (var id in _jobs.Values)
        {
            var folder = Path.Combine(Data, "jobs", id.Value);
            if (!Directory.Exists(folder)) continue;
            foreach (var file in Directory.GetFiles(folder)) File.Copy(file, Path.Combine(output, id.Value + "-" + Path.GetFileName(file)), true);
        }
        _say($"{_id}: logs kept in {output}");
        return Task.CompletedTask;
    }
}
