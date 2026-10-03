using System.Collections.Generic;

namespace Bromelia.Domain;

/// <summary>A program and its arguments. A bare program name is resolved by the adapter (ToolLocator).</summary>
public sealed record CommandLine(string Executable, IReadOnlyList<string> Arguments);
