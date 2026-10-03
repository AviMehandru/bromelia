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

    public static JsonValue Json(string relative) =>
        JsonValue.Parse(File.ReadAllBytes(Path_(relative))) ?? throw new InvalidOperationException(relative + " isn't JSON");

    /// <summary>Runs every case of a <c>*.cases.json</c> file. <paramref name="run"/> returns false for a case
    /// it doesn't know, which fails the test: no case is skipped silently.</summary>
    public static void RunCases(string relative, Func<string, JsonValue, JsonValue, bool> run)
    {
        var doc = Json(relative);
        var failures = new List<string>();
        int count = 0;
        foreach (var c in doc["cases"]!.AsArray!)
        {
            var id = c["id"]!.AsString!;
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
