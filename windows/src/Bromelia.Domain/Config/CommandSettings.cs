using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>A command step: the program, its interpreter (empty = run it directly), the arguments (split like a
/// POSIX command line before tokens are filled in), the working directory (empty = the unit's folder), whether it
/// runs once per file, and its environment.</summary>
public sealed record CommandSettings(
    string Executable = "",
    string Interpreter = "",
    string Arguments = "{outputDir}",
    string WorkingDirectory = "",
    bool PerFile = false,
    IReadOnlyDictionary<string, string>? Environment = null);
