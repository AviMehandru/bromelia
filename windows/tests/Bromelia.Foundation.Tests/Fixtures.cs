using System.Text;
using Bromelia.Foundation;
using Xunit;

namespace Bromelia.Tests;

/// <summary>Reads the shared golden fixtures (shared/fixtures) and runs their cases.</summary>
public static class Fixtures
{
    public static readonly string Root = FindRoot();

    // BROMELIA_FIXTURES (as on Linux), else the repository this file was compiled from, else above the binary.
    private static string FindRoot([System.Runtime.CompilerServices.CallerFilePath] string source = "")
    {
        if (Environment.GetEnvironmentVariable("BROMELIA_FIXTURES") is { Length: > 0 } env) return env;
        foreach (var start in new[] { Path.GetDirectoryName(source), AppContext.BaseDirectory })
            for (var dir = start is null ? null : new DirectoryInfo(start); dir != null; dir = dir.Parent)
            {
                var candidate = Path.Combine(dir.FullName, "shared", "fixtures");
                if (Directory.Exists(candidate)) return candidate;
            }
        throw new InvalidOperationException("shared/fixtures not found above " + source + " or " + AppContext.BaseDirectory);
    }

    public static string Path_(string relative) => Path.Combine(Root, relative.Replace('/', Path.DirectorySeparatorChar));

    public static string Text(string relative) => File.ReadAllText(Path_(relative), Encoding.UTF8);

    /// <summary>A file elsewhere in shared/ (schema/common.json, messages/codes.json).</summary>
    public static JsonValue SharedJson(string relative) => Json("../" + relative);

    public static JsonValue Json(string relative) =>
        JsonValue.Parse(File.ReadAllBytes(Path_(relative))) ?? throw new InvalidOperationException(relative + " isn't JSON");

    /// <summary>Runs every case of a <c>*.cases.json</c> file. <paramref name="run"/> returns false for a case
    /// it doesn't know, which fails the test: no case is skipped silently.</summary>
    public static void RunCases(string relative, Func<string, JsonValue, JsonValue, bool> run) => RunCases(relative, _ => true, run);

    /// <summary>Runs the cases of a file whose id passes <paramref name="only"/>: for a file whose other cases
    /// belong to a module that isn't built yet.</summary>
    public static void RunCases(string relative, Func<string, bool> only, Func<string, JsonValue, JsonValue, bool> run)
    {
        var doc = Json(relative);
        var failures = new List<string>();
        int count = 0;
        foreach (var c in doc["cases"]!.AsArray!)
        {
            var id = c["id"]!.AsString!;
            if (!only(id)) continue;
            count++;
            try
            {
                if (!run(id, c["given"]!, c["expect"]!)) failures.Add($"{id}: no test handles this case");
            }
            catch (Exception e)
            {
                failures.Add($"{id}: {e.Message}");
            }
        }
        Assert.True(count > 0, relative + " has no cases");
        Assert.True(failures.Count == 0, $"{relative}: {failures.Count} of {count} case(s) failed:\n" + string.Join("\n", failures));
    }

    /// <summary>The robot-mode listing of a disc definition (shared/scenarios/README.md, "A generated listing"):
    /// <c>{volume, type, titles}</c>, each title <c>{duration, source, size, chapters}</c> or <c>[duration, source]</c>.</summary>
    public static string GeneratedListing(JsonValue disc)
    {
        var volume = disc["volume"]?.AsString ?? "";
        var type = disc["type"]?.AsString ?? "bluray";
        var titles = disc["titles"]?.AsArray ?? new List<JsonValue>();
        var sb = new StringBuilder();
        sb.Append("MSG:1005,0,1,\"MakeMKV v1.18.1 darwin(arm64-release) started\",\"%1 started\",\"MakeMKV v1.18.1 darwin(arm64-release)\"\n");
        sb.Append($"TCOUNT:{titles.Count}\n");
        sb.Append(type switch { "dvd" => "CINFO:1,6206,\"DVD disc\"\n", "hddvd" => "CINFO:1,6207,\"HD-DVD disc\"\n", _ => "CINFO:1,6209,\"Blu-ray disc\"\n" });
        sb.Append($"CINFO:2,0,\"{volume}\"\nCINFO:32,0,\"{volume}\"\n");
        for (int i = 0; i < titles.Count; i++)
        {
            var t = titles[i];
            string duration = t.AsArray is { } pair ? pair[0].AsString! : t["duration"]!.AsString!;
            long source = t.AsArray is { } p2 ? p2[1].AsInteger!.Value : t["source"]!.AsInteger!.Value;
            long? size = t.AsArray is null ? t["size"]?.AsInteger : null;
            long chapters = t.AsArray is null ? t["chapters"]?.AsInteger ?? 2 : 2;
            sb.Append($"TINFO:{i},8,0,\"{chapters}\"\nTINFO:{i},9,0,\"{duration}\"\nTINFO:{i},16,0,\"0000{source}.mpls\"\n");
            sb.Append($"TINFO:{i},24,0,\"{source}\"\nTINFO:{i},26,0,\"{source}\"\nTINFO:{i},27,0,\"title_t0{i}.mkv\"\n");
            if (size is { } s) sb.Append($"TINFO:{i},11,0,\"{s}\"\n");
            sb.Append($"SINFO:{i},0,1,6201,\"Video\"\n");
            if (type == "uhd") sb.Append($"SINFO:{i},0,19,0,\"3840x2160\"\n");
            sb.Append($"SINFO:{i},1,1,6202,\"Audio\"\n");
        }
        sb.Append("MSG:5011,0,0,\"Operation successfully completed\",\"Operation successfully completed\"\n");
        return sb.ToString();
    }

    /// <summary>The value with every object's keys sorted, to compare documents whose key order doesn't matter.</summary>
    public static JsonValue Sorted(JsonValue v) => v switch
    {
        JsonValue.Object o => new JsonValue.Object(o.Members.OrderBy(m => m.Key, StringComparer.Ordinal)
            .Select(m => new KeyValuePair<string, JsonValue>(m.Key, Sorted(m.Value))).ToList()),
        JsonValue.Array a => new JsonValue.Array(a.Items.Select(Sorted).ToList()),
        _ => v,
    };

    /// <summary>The value at a dotted path (a.b.c), or null.</summary>
    public static JsonValue? At(JsonValue v, string path)
    {
        JsonValue? cur = v;
        foreach (var part in path.Split('.')) cur = cur?[part];
        return cur;
    }

    /// <summary>Assert.Equal with a message that names the case's value.</summary>
    public static void Same<T>(T expected, T actual, string what)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            throw new Xunit.Sdk.XunitException($"{what}: expected {Show(expected)}, got {Show(actual)}");
    }

    private static string Show(object? o) => o switch
    {
        null => "null",
        JsonValue j => j.ToString().TrimEnd('\n'),
        string s => "\"" + s + "\"",
        System.Collections.IEnumerable e => "[" + string.Join(", ", e.Cast<object?>().Select(Show)) + "]",
        _ => o.ToString() ?? "null",
    };
}
