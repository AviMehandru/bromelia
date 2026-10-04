using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A process to start, never through a shell: the program and its arguments, environment variables added to
/// the engine's, the working directory, how it is stopped, how long it may stay silent before it is stopped as stalled,
/// the transcript file every line is appended to (headed by the command line), and the interpreter for a
/// script.</summary>
public sealed record ProcessSpec(
    string Executable,
    IReadOnlyList<string> Arguments,
    IReadOnlyDictionary<string, string> Environment,
    string? WorkingDirectory,
    StopPolicy StopPolicy,
    Duration? StallTimeout = null,
    string? Transcript = null,
    string? Interpreter = null);
