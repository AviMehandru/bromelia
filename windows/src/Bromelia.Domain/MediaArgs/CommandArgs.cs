using System.Collections.Generic;
using System.Linq;

namespace Bromelia.Domain;

/// <summary>The command line of a command step.</summary>
public static class CommandArgs
{
    /// <summary>The arguments are split first and filled in afterwards, so a value with spaces stays one argument,
    /// and a lone <c>{files}</c> becomes one argument per file. With an interpreter, the program is its first
    /// argument. A leading ~ is <paramref name="home"/>. (Choosing an interpreter by extension when none is set is
    /// the process launcher's: it depends on the system and the file's mode.)</summary>
    public static CommandLine Build(StepDefinition step, IReadOnlyDictionary<string, string> values, IReadOnlyList<string> files, string? home)
    {
        var c = step.Command ?? new CommandSettings();
        var args = new List<string>();
        foreach (var token in ArgumentSplitter.Split(c.Arguments))
        {
            if (token == "{files}") args.AddRange(files);
            else args.Add(TemplateEngine.Render(token, values));
        }
        var exe = ExpandHome(TemplateEngine.Render(c.Executable, values), home);
        var interpreter = ExpandHome(c.Interpreter, home);
        return interpreter.Length > 0 ? new CommandLine(interpreter, new[] { exe }.Concat(args).ToList()) : new CommandLine(exe, args);
    }

    /// <summary><c>~</c> and <c>~/…</c> (or <c>~\…</c>) start in <paramref name="home"/>; anything else, or no home,
    /// stays as it is.</summary>
    internal static string ExpandHome(string path, string? home)
    {
        if (home == null || path.Length == 0 || path[0] != '~') return path;
        if (path.Length == 1) return home;
        return path[1] is '/' or '\\' ? home.TrimEnd('/', '\\') + path.Substring(1) : path;
    }
}
