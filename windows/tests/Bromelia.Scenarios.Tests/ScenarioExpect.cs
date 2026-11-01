using System.Text;
using System.Text.RegularExpressions;
using Bromelia.Adapters;
using Bromelia.Domain;
using Bromelia.Engine;
using Bromelia.Foundation;
using Bromelia.Tests;
using Microsoft.Data.Sqlite;

namespace Bromelia.Scenarios.Tests;

/// <summary>A scenario's step didn't hold: what was expected, and what was found.</summary>
public sealed class ScenarioFailure : Exception
{
    public ScenarioFailure(string expected, string found) : base(expected + " / " + found) { }

    public ScenarioFailure(string message) : base(message) { }
}

/// <summary>
/// The expectations of an <c>expect</c> step (decisions 238, 241, 243), one function per key, each reading what the table
/// of decision 238 gives. Without <c>when</c> the step first settles (decision 239); with it, it waits for the condition and
/// never moves the clock. Placeholders: {root}, {unit8} and {jobId} of the job the step names (else the only job),
/// {unit8:&lt;name&gt;}, {jobId:&lt;name&gt;}, {driveId:&lt;drive&gt;}.
/// </summary>
public static class ScenarioExpect
{
    static readonly MessageTextRenderer English = new(EnglishMessages.Catalog);

    public static async Task Check(ScenarioRun run, JsonValue expect)
    {
        if (expect["when"] is { } when) await When(run, when);
        else await run.Settle();
        var named = expect["job"]?["name"]?.AsString;
        foreach (var (key, want) in expect.AsObject!)
        {
            switch (key)
            {
                case "when" or "note":
                    break;
                case "job":
                    await ExpectJob(run, Fill(run, want, named));
                    break;
                case "jobs":
                    await ExpectJobs(run, want);
                    break;
                case "files":
                    ExpectFiles(run, Fill(run, want, named));
                    break;
                case "file":
                    ExpectFile(run, Fill(run, want, named));
                    break;
                case "sums":
                    ExpectSums(run, Fill(run, want, named));
                    break;
                case "record":
                    ExpectRecord(run, want, named);
                    break;
                case "transcript":
                    ExpectTranscript(run, Fill(run, want, named), named);
                    break;
                case "debugLog":
                    ExpectDebugLog(run, Fill(run, want, named), named);
                    break;
                case "calls":
                    ExpectCalls(run, Fill(run, want, named));
                    break;
                case "log":
                    await ExpectLog(run, Fill(run, want, named), named);
                    break;
                case "events":
                    ExpectEvents(run, Fill(run, want, named));
                    break;
                case "problems":
                    await ExpectProblems(run, Fill(run, want, named));
                    break;
                case "drives":
                    await ExpectDrives(run, Fill(run, want, named));
                    break;
                case "db":
                    ExpectDb(run, Fill(run, want, named));
                    break;
                case "config":
                    ExpectConfig(run, Fill(run, want, named));
                    break;
                case "manifest" or "notifications":
                    throw new ScenarioFailure($"{key} (phase 5)", "not built");
                case "session" or "power" or "response":
                    throw new ScenarioFailure($"{key} (phase 4)", "not built");
                default:
                    throw new ScenarioFailure("a known expectation", key);
            }
        }
    }

    // ---- when ----

    /// <summary>Waits (never moving the clock) until the job's step has started, the job is in the state (finished: it has
    /// an outcome) or has the outcome, or an event of the type has been published.</summary>
    public static async Task When(ScenarioRun run, JsonValue when)
    {
        var deadline = DateTime.UtcNow + ScenarioRun.Patience;
        while (true)
        {
            if (await Holds(run, when)) return;
            if (DateTime.UtcNow > deadline) throw new ScenarioFailure("when " + ScenarioRun.JsonText(when), "it never held within 10 s");
            await Task.Delay(5);
        }
    }

    static async Task<bool> Holds(ScenarioRun run, JsonValue when)
    {
        if (when["event"]?.AsString is { } type) return run.Events().Any(e => EnumWire.Name(e.Type) == type);
        var detail = await run.Root.Engine.Job(run.Job(when["job"]!.AsString!));
        if (when["step"]?.AsString is { } step && !detail.Steps.Any(s => EnumWire.Name(s.Kind) == step && s.StartedAt != null)) return false;
        if (when["state"]?.AsString is { } state && EnumWire.Name(detail.Job.State) != state) return false;
        if (when["outcome"]?.AsString is { } outcome && (detail.Job.Outcome is not { } o || EnumWire.Name(o) != outcome)) return false;
        return true;
    }

    // ---- job, jobs ----

    public static async Task ExpectJob(ScenarioRun run, JsonValue want)
    {
        var name = want["name"]?.AsString ?? OnlyJob(run);
        var id = run.Job(name);
        var detail = await run.Root.Engine.Job(id);
        try
        {
            await ExpectJob(run, want, name, id, detail);
        }
        catch (ScenarioFailure f)
        {
            var log = (await run.Root.Engine.JobLog(id, 0)).Select(l => MessageCode.Wire(l.Code) + (l.Params.Members.Count > 0 ? " " + ScenarioRun.JsonText(l.Params) : ""));
            throw new ScenarioFailure(f.Message + $"\n  (job {name}: {EnumWire.Name(detail.Job.State)}, error {ScenarioRun.JsonText(JobDetailDto.Json(detail)["error"])}, steps "
                + string.Join(" ", detail.Steps.Select(st => EnumWire.Name(st.Kind) + ":" + EnumWire.Name(st.State))) + ", log:\n    "
                + string.Join("\n    ", log) + ")");
        }
    }

    static async Task ExpectJob(ScenarioRun run, JsonValue want, string name, Id id, JobDetailDto detail)
    {
        var json = JobDetailDto.Json(detail);
        string what = "job " + name;
        foreach (var (key, value) in want.AsObject!)
        {
            switch (key)
            {
                case "name":
                    break;
                case "state" or "outcome" or "mode" or "kind" or "queue" or "automatic":
                    Same(value, json[key], $"{what} {key}");
                    break;
                case "error" or "blockedBy" or "decision":
                    Same(value, json[key], $"{what} {key}");
                    break;
                case "noError":
                    if (value.AsBool == true && json["error"] != null) throw new ScenarioFailure($"{what} without an error", ScenarioRun.JsonText(json["error"]));
                    break;
                case "errorText":
                {
                    var text = json["error"] is { } error ? English.RenderJson(error) : "";
                    if (!text.Contains(value.AsString!, StringComparison.Ordinal))
                        throw new ScenarioFailure($"{what} error text containing \"{value.AsString}\"", $"\"{text}\"");
                    break;
                }
                case "parent":
                    Same(JsonValue.Of(run.Job(value.AsString!).Value), json["parentId"], $"{what} parent");
                    break;
                case "steps":
                    Same(value, JsonValue.Of(detail.Steps.Where(s => s.State != StepState.Pending).Select(s => JsonValue.Of(EnumWire.Name(s.Kind)))),
                        $"{what} steps");
                    break;
                case "files":
                    Same(value, JsonValue.Of(detail.Files.Count), $"{what} files");
                    break;
                case "titles":
                {
                    var rows = await run.Root.Store.Steps().Load(id);
                    var acquire = rows.LastOrDefault(r => r.Kind == Domain.StepKind.Acquire && r.Output != null);
                    var titles = acquire == null ? new List<JsonValue>()
                        : AcquireOutput.Decode(acquire.Output!.Data).Titles.Select(t => JsonValue.Of(t.Index)).ToList();
                    Same(value, JsonValue.Of(titles), $"{what} titles");
                    break;
                }
                case "unit":
                {
                    var unit = detail.Job.UnitId ?? ArchiveFiles.UnitId(id);
                    var rows = Db(run, "archive_units").Where(r => r["id"]?.AsString == unit.Value).ToList();
                    if (value.AsObject!.Count == 0) break;
                    if (rows.Count == 0) throw new ScenarioFailure($"{what} unit {ScenarioRun.JsonText(value)}", "no archive_units row for " + unit.Value);
                    Same(value, rows[0], $"{what} unit");
                    break;
                }
                case "counts":
                {
                    var last = run.Events().LastOrDefault(e => e.Type is EngineEventType.JobChanged or EngineEventType.JobFinished
                                                               && e.Data["job"]?["id"]?.AsString == id.Value);
                    Same(value, last?.Data["job"]?["counts"] ?? JsonValue.Null.Instance, $"{what} counts (its last jobChanged or jobFinished)");
                    break;
                }
                default:
                    throw new ScenarioFailure("a known job expectation", key);
            }
        }
    }

    static string OnlyJob(ScenarioRun run) => run.Jobs.Count == 1 ? run.Jobs.Keys.First() : throw new ScenarioFailure("one job, or a name", run.Jobs.Count + " jobs");

    public static async Task ExpectJobs(ScenarioRun run, JsonValue want)
    {
        var jobs = await run.AllJobs();
        if (want["count"]?.AsInteger is { } count) Same(JsonValue.Of(count), JsonValue.Of(jobs.Count), "jobs count");
        if (want["outcomes"] is { } outcomes)
            Same(outcomes, JsonValue.Of(jobs.Select(j => j.Outcome is { } o ? JsonValue.Of(EnumWire.Name(o)) : JsonValue.Null.Instance)), "jobs outcomes");
        if (want["states"] is { } states) Same(states, JsonValue.Of(jobs.Select(j => JsonValue.Of(EnumWire.Name(j.State)))), "jobs states");
        if (want["kinds"] is { } kinds) Same(kinds, JsonValue.Of(jobs.Select(j => JsonValue.Of(EnumWire.Name(j.Kind)))), "jobs kinds");
    }

    // ---- files ----

    /// <summary>Every file and folder under the folder, as paths relative to it (folders without a final '/').</summary>
    static (List<string> Files, List<string> Folders) Walk(ScenarioRun run, string folder)
    {
        var files = new List<string>();
        var folders = new List<string>();
        void Visit(string dir, string rel)
        {
            foreach (var e in run.Root.Fs.List(dir))
            {
                var r = rel.Length == 0 ? e.Name : rel + "/" + e.Name;
                if (e.IsDirectory)
                {
                    folders.Add(r);
                    Visit(dir + "/" + e.Name, r);
                }
                else files.Add(r);
            }
        }
        if (run.Root.Fs.Exists(folder)) Visit(folder, "");
        return (files, folders);
    }

    static bool Hidden(string path) => path.Split('/').Any(p => p.StartsWith('.'));

    public static void ExpectFiles(ScenarioRun run, JsonValue want)
    {
        var under = ScenarioRun.Path(want["under"]?.AsString ?? "");
        var (files, folders) = Walk(run, under.TrimEnd('/'));
        var all = files.Concat(folders).ToList();
        string where = want["under"]?.AsString is { } u ? " under " + u : "";
        if (want["exactly"]?.AsArray is { } exactly)
        {
            // The non-hidden files, and every non-hidden empty folder as name/ (a job that saved nothing leaves no folder).
            var listed = files.Where(f => !Hidden(f))
                .Concat(folders.Where(d => !Hidden(d) && !all.Any(x => x.StartsWith(d + "/", StringComparison.Ordinal))).Select(d => d + "/"))
                .OrderBy(x => x, StringComparer.Ordinal).ToList();
            var patterns = exactly.Select(p => p.AsString!).ToList();
            bool ok = listed.All(f => patterns.Any(p => Glob(p, f))) && patterns.All(p => listed.Any(f => Glob(p, f)));
            if (!ok) throw new ScenarioFailure($"exactly {Show(patterns.OrderBy(x => x, StringComparer.Ordinal))}{where}", Show(listed));
        }
        foreach (var p in want["include"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (!all.Any(f => Glob(p, f)))
                throw new ScenarioFailure($"{p}{where}", Show(all.Where(f => !Hidden(f) || p.Contains('.')).OrderBy(x => x, StringComparer.Ordinal)));
        foreach (var p in want["exclude"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (all.Where(f => Glob(p, f)).ToList() is { Count: > 0 } found)
                throw new ScenarioFailure($"no {p}{where}", Show(found));
    }

    static string Show(IEnumerable<string> items) => "[" + string.Join(", ", items.Select(i => "\"" + i + "\"")) + "]";

    /// <summary>A path pattern: * within one component, ** across components; a component starting with '.' is matched only
    /// by a pattern component that writes the dot.</summary>
    public static bool Glob(string pattern, string path)
    {
        var p = pattern.TrimEnd('/');
        var parts = path.TrimEnd('/').Split('/');
        var pats = p.Split('/');
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

    public static void ExpectFile(ScenarioRun run, JsonValue want)
    {
        var rel = want["path"]!.AsString!;
        var path = ScenarioRun.Path(rel);
        var fs = run.Root.Fs;
        if (want["exists"]?.AsBool is { } exists && fs.Exists(path) != exists)
            throw new ScenarioFailure(exists ? rel : "no " + rel, fs.Exists(path) ? "it is there" : "it isn't there");
        if (want.AsObject!.All(m => m.Key is "path" or "exists")) return;
        if (!fs.Exists(path) || fs.Stat(path).IsDirectory) throw new ScenarioFailure("the file " + rel, "none");
        var bytes = fs.Read(path);
        var text = Encoding.UTF8.GetString(bytes);
        if (want["text"]?.AsString is { } wanted)
        {
            var m = Regex.Match(wanted, "^<<shared/fixtures/(.+)>>$");
            var expected = m.Success ? Fixtures.Text(m.Groups[1].Value) : wanted;
            if (text != expected) throw new ScenarioFailure($"{rel} reading \"{expected}\"", $"\"{text}\"");
        }
        foreach (var c in want["contains"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (!text.Contains(c, StringComparison.Ordinal)) throw new ScenarioFailure($"{rel} containing \"{c}\"", $"\"{text}\"");
        foreach (var c in want["excludes"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (text.Contains(c, StringComparison.Ordinal)) throw new ScenarioFailure($"{rel} without \"{c}\"", $"\"{text}\"");
        if (want["json"] is { } json) Same(json, JsonValue.Parse(bytes) ?? JsonValue.Null.Instance, rel);
        if (want["sha256"]?.AsString is { } sha && Sha256Sums.Hash(bytes) != sha) throw new ScenarioFailure($"{rel} sha256 {sha}", Sha256Sums.Hash(bytes));
        if (want["sameAsDisc"] != null) throw new ScenarioFailure("file.sameAsDisc (phase 5)", "not built");
    }

    /// <summary>SHA256SUMS in the folder, every file it lists hashed again; files: exactly the names it lists.</summary>
    public static void ExpectSums(ScenarioRun run, JsonValue want)
    {
        var rel = want["folder"]!.AsString!;
        var folder = ScenarioRun.Path(rel);
        var fs = run.Root.Fs;
        var sums = folder + "/" + Sha256Sums.FileName;
        if (!fs.Exists(sums)) throw new ScenarioFailure($"{rel}/{Sha256Sums.FileName}", "none");
        var entries = Sha256Sums.Parse(Encoding.UTF8.GetString(fs.Read(sums)));
        var bad = new List<string>();
        foreach (var e in entries)
        {
            var path = folder + "/" + e.Path;
            if (!fs.Exists(path)) bad.Add(e.Path + " missing");
            else
            {
                var hasher = new Sha256Hasher();
                hasher.Update(fs.Read(path));
                if (hasher.Finish() != e.Sha256) bad.Add(e.Path + " differs");
            }
        }
        if (want["ok"]?.AsBool is { } ok && (bad.Count == 0) != ok)
            throw new ScenarioFailure($"sha256sum -c in {rel} {(ok ? "passing" : "failing")}", bad.Count == 0 ? "it passes" : string.Join(", ", bad));
        if (want["files"] is { } files)
            Same(JsonValue.Of(files.AsArray!.Select(f => f.AsString!).OrderBy(x => x, StringComparer.Ordinal).Select(x => JsonValue.Of(x))),
                JsonValue.Of(entries.Select(e => e.Path).OrderBy(x => x, StringComparer.Ordinal).Select(x => JsonValue.Of(x))), $"the files {rel}/SHA256SUMS lists");
    }

    public static void ExpectRecord(ScenarioRun run, JsonValue want, string? named)
    {
        var name = want["job"]?.AsString ?? named ?? OnlyJob(run);
        var file = "bromelia-" + ArchiveFiles.UnitId(run.Job(name)).Value[..8] + ".json";
        string rel, path;
        if (want["folder"] is { } folder)
        {
            rel = Fill(run, folder, name).AsString!;
            path = ScenarioRun.Path(rel) + "/" + file;
        }
        else
        {
            // The unit's folder, wherever it went: the one record of that name under the root.
            var found = Walk(run, TestCompositionRoot.Root).Files.Where(f => f == file || f.EndsWith("/" + file, StringComparison.Ordinal)).ToList();
            if (found.Count != 1) throw new ScenarioFailure("one record " + file, Show(found));
            rel = found[0];
            path = TestCompositionRoot.Root + "/" + rel;
        }
        if (!run.Root.Fs.Exists(path)) throw new ScenarioFailure("the record " + path, "none");
        Same(Fill(run, want["json"] ?? JsonValue.Of(), name), JsonValue.Parse(run.Root.Fs.Read(path)) ?? JsonValue.Null.Instance, "the record in " + rel);
    }

    /// <summary>The job folder's MakeMKV transcript: its runs (==== … $ headers) and the lines it contains.</summary>
    public static void ExpectTranscript(ScenarioRun run, JsonValue want, string? named) =>
        Transcript(run, want, named, SealStep.Transcript, "transcript");

    public static void ExpectDebugLog(ScenarioRun run, JsonValue want, string? named) =>
        Transcript(run, want, named, SealStep.DebugTranscript, "debug log");

    static void Transcript(ScenarioRun run, JsonValue want, string? named, string file, string what)
    {
        var name = want["job"]?.AsString ?? named ?? OnlyJob(run);
        var path = TestCompositionRoot.DataDir + "/jobs/" + run.Job(name).Value + "/" + file;
        if (!run.Root.Fs.Exists(path)) throw new ScenarioFailure($"the {what} of {name}", "none");
        var text = Encoding.UTF8.GetString(run.Root.Fs.Read(path));
        var lines = text.Split('\n');
        if (want["runs"]?.AsInteger is { } runs)
        {
            var found = lines.Count(l => l.StartsWith("==== ", StringComparison.Ordinal) && l.Contains(" $ ", StringComparison.Ordinal));
            if (found != runs) throw new ScenarioFailure($"{runs} runs in the {what} of {name}", found + ":\n" + text);
        }
        foreach (var c in want["contains"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (!text.Contains(c, StringComparison.Ordinal)) throw new ScenarioFailure($"the {what} of {name} containing \"{c}\"", text);
        int at = 0;
        foreach (var c in want["order"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
        {
            int i = text.IndexOf(c, at, StringComparison.Ordinal);
            if (i < 0) throw new ScenarioFailure($"the {what} of {name} containing \"{c}\" after what came before", text);
            at = i + c.Length;
        }
    }

    // ---- calls, log, events, problems ----

    public static void ExpectCalls(ScenarioRun run, JsonValue want)
    {
        foreach (var (tool, w) in want.AsObject!)
        {
            var calls = run.Tools.Calls(tool);
            string Found() => calls.Count + " calls: " + string.Join("\n  ", calls);
            if (w["none"]?.AsBool == true && calls.Count > 0) throw new ScenarioFailure($"no {tool} call", Found());
            if (w["count"]?.AsInteger is { } count && calls.Count != count) throw new ScenarioFailure($"{count} {tool} calls", Found());
            foreach (var s in w["include"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
                if (!calls.Any(c => c.Contains(s, StringComparison.Ordinal))) throw new ScenarioFailure($"a {tool} call with \"{s}\"", Found());
            foreach (var s in w["exclude"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
                if (calls.Any(c => c.Contains(s, StringComparison.Ordinal))) throw new ScenarioFailure($"no {tool} call with \"{s}\"", Found());
            foreach (var (s, n) in w["counts"]?.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
                if (calls.Count(c => c.Contains(s, StringComparison.Ordinal)) != n.AsInteger)
                    throw new ScenarioFailure($"{n.AsInteger} {tool} calls with \"{s}\"", Found());
        }
    }

    public static async Task ExpectLog(ScenarioRun run, JsonValue want, string? named)
    {
        var name = want["job"]?.AsString ?? named ?? OnlyJob(run);
        var codes = (await run.Root.Engine.JobLog(run.Job(name), 0)).Select(l => MessageCode.Wire(l.Code)).ToList();
        foreach (var c in want["include"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (!codes.Contains(c)) throw new ScenarioFailure($"{c} in the log of {name}", Show(codes));
        foreach (var c in want["exclude"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (codes.Contains(c)) throw new ScenarioFailure($"no {c} in the log of {name}", Show(codes));
    }

    /// <summary>An event as an expectation reads it: its data's members and its type.</summary>
    static JsonValue EventJson(EngineEvent e) =>
        new JsonValue.Object(((e.Data as JsonValue.Object)?.Members ?? new List<KeyValuePair<string, JsonValue>>())
            .Append(new("type", JsonValue.Of(EnumWire.Name(e.Type)))).ToList());

    public static void ExpectEvents(ScenarioRun run, JsonValue want)
    {
        var events = run.Events();
        var types = events.Select(e => EnumWire.Name(e.Type)).ToList();
        foreach (var w in want["include"]?.AsArray ?? new List<JsonValue>())
            if (!events.Any(e => Matches(w, EventJson(e)))) throw new ScenarioFailure("an event " + ScenarioRun.JsonText(w), Show(types));
        int at = 0;
        foreach (var t in want["sequence"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
        {
            int i = types.IndexOf(t, at);
            if (i < 0) throw new ScenarioFailure($"{t} in the sequence {ScenarioRun.JsonText(want["sequence"])}", Show(types));
            at = i + 1;
        }
        foreach (var t in want["exclude"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (types.Contains(t)) throw new ScenarioFailure("no " + t, Show(types));
    }

    public static async Task ExpectProblems(ScenarioRun run, JsonValue want)
    {
        var active = (await run.Root.Engine.System()).Problems.Select(p => MessageCode.Wire(p.Message.Code)).ToList();
        if (want["none"]?.AsBool == true && active.Count > 0) throw new ScenarioFailure("no problems", Show(active));
        foreach (var c in want["active"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
            if (!active.Contains(c)) throw new ScenarioFailure("the problem " + c, Show(active));
        foreach (var c in want["cleared"]?.AsArray?.Select(x => x.AsString!) ?? Enumerable.Empty<string>())
        {
            if (active.Contains(c)) throw new ScenarioFailure($"{c} cleared", "still active");
            if (!run.Events().Any(e => e.Type == EngineEventType.ProblemCleared && e.Data.ToString().Contains("\"" + c + "\"", StringComparison.Ordinal)))
                throw new ScenarioFailure($"a problemCleared for {c}", "none");
        }
    }

    /// <summary>Each drive by its scenario id: a fake drive's through its device, else a configured one's through its
    /// configId.</summary>
    public static async Task ExpectDrives(ScenarioRun run, JsonValue want)
    {
        var drives = (await run.Root.Engine.Drives()).ToList();
        foreach (var w in want.AsArray!)
        {
            var id = w["id"]!.AsString!;
            var device = run.World.Drives.FirstOrDefault(d => d.Name == id)?.Device;
            var drive = drives.FirstOrDefault(d => device != null ? d.Device == device : d.ConfigId == id)
                ?? throw new ScenarioFailure("the drive " + id, ScenarioRun.JsonText(JsonValue.Of(drives.Select(DriveDto.Json))));
            var subset = new JsonValue.Object(w.AsObject!.Where(m => m.Key != "id").ToList());
            Same(subset, DriveDto.Json(drive), "the drive " + id);
        }
    }

    public static void ExpectConfig(ScenarioRun run, JsonValue want)
    {
        var envelope = run.Root.Engine.Config().GetAwaiter().GetResult();
        Same(want, envelope.Config.Document, "the configuration");
    }

    // ---- db ----

    /// <summary>A table's rows, in rowid order, through a read-only connection of its own.</summary>
    static List<JsonValue> Db(ScenarioRun run, string table)
    {
        if (!Regex.IsMatch(table, "^[a-z_]+$")) throw new ScenarioFailure("a table name", table);
        using var db = new SqliteConnection(new SqliteConnectionStringBuilder
        {
            DataSource = Path.Combine(run.Root.Folder, "bromelia.db"), Mode = SqliteOpenMode.ReadOnly, Pooling = false,
        }.ToString());
        db.Open();
        using var c = db.CreateCommand();
        c.CommandText = $"SELECT * FROM {table} ORDER BY rowid";
        using var r = c.ExecuteReader();
        var rows = new List<JsonValue>();
        while (r.Read())
        {
            var m = new List<KeyValuePair<string, JsonValue>>();
            for (int i = 0; i < r.FieldCount; i++)
                m.Add(new(r.GetName(i), r.IsDBNull(i) ? JsonValue.Null.Instance : r.GetValue(i) switch
                {
                    long l => JsonValue.Of(l),
                    double d => new JsonValue.Number(d),
                    byte[] b => JsonValue.Of(Convert.ToHexString(b).ToLowerInvariant()),
                    var o => JsonValue.Of(Convert.ToString(o, System.Globalization.CultureInfo.InvariantCulture)),
                }));
            rows.Add(new JsonValue.Object(m));
        }
        return rows;
    }

    public static void ExpectDb(ScenarioRun run, JsonValue want)
    {
        foreach (var (table, w) in want.AsObject!)
        {
            var rows = Db(run, table);
            if (w["count"]?.AsInteger is { } count && rows.Count != count)
                throw new ScenarioFailure($"{count} rows in {table}", rows.Count + ": " + ScenarioRun.JsonText(JsonValue.Of(rows)));
            if (w["rows"] is { } wanted) Same(wanted, JsonValue.Of(rows), "the rows of " + table);
        }
    }

    // ---- matching and placeholders ----

    /// <summary>{root}, {unit8} / {jobId} (of the job named in the step, else the only job), {unit8:&lt;name&gt;},
    /// {jobId:&lt;name&gt;}, {driveId:&lt;drive&gt;} in every string.</summary>
    public static JsonValue Fill(ScenarioRun run, JsonValue v, string? named) => v switch
    {
        JsonValue.String s => JsonValue.Of(Fill(run, s.Value, named)),
        JsonValue.Array a => JsonValue.Of(a.Items.Select(x => Fill(run, x, named))),
        JsonValue.Object o => new JsonValue.Object(o.Members.Select(m => new KeyValuePair<string, JsonValue>(m.Key, Fill(run, m.Value, named))).ToList()),
        _ => v,
    };

    public static string Fill(ScenarioRun run, string s, string? named)
    {
        s = s.Replace("{root}", TestCompositionRoot.Root);
        s = Regex.Replace(s, @"\{(unit8|jobId)(?::([^}]+))?\}", m =>
        {
            var name = m.Groups[2].Success ? m.Groups[2].Value : named ?? OnlyJob(run);
            var id = run.Job(name);
            return m.Groups[1].Value == "jobId" ? id.Value : ArchiveFiles.UnitId(id).Value[..8];
        });
        return Regex.Replace(s, @"\{driveId:([^}]+)\}", m => run.DriveId(m.Groups[1].Value).GetAwaiter().GetResult());
    }

    static void Same(JsonValue want, JsonValue? got, string what)
    {
        if (!Matches(want, got)) throw new ScenarioFailure($"{what} {ScenarioRun.JsonText(want)}", ScenarioRun.JsonText(got));
    }

    /// <summary>Objects: every wanted member matches (a wanted null: absent or null). Arrays: the same length, element by
    /// element. Numbers by value. A wanted string with '*' in it: a glob ('*' any run of characters; owner H3b-5). Anything
    /// else: equal.</summary>
    public static bool Matches(JsonValue want, JsonValue? got) => want switch
    {
        JsonValue.Null => got is null or JsonValue.Null,
        JsonValue.String { Value: var w } when w.Contains('*') => got is JsonValue.String { Value: var g }
            && Regex.IsMatch(g, "^" + Regex.Escape(w).Replace("\\*", ".*") + "$", RegexOptions.Singleline),
        JsonValue.Object o => got is JsonValue.Object && o.Members.All(m => Matches(m.Value, got[m.Key])),
        JsonValue.Array a => got is JsonValue.Array ga && ga.Items.Count == a.Items.Count && a.Items.Zip(ga.Items).All(p => Matches(p.First, p.Second)),
        JsonValue.Integer or JsonValue.Number => got?.AsNumber is { } n && want.AsNumber == n,
        _ => Equals(want, got),
    };
}
