using System.Collections.Generic;

namespace Bromeliad;

/// <summary>What the graph is built for. There are no defaults: the platform's data folder holds the real config.json
/// and secrets, so a test always passes its own.</summary>
/// <param name="DataDir">The data folder: bromelia.db, jobs, secrets, logs (made when it isn't there).</param>
/// <param name="ConfigPath">config.json; null: &lt;data&gt;/config.json.</param>
/// <param name="Home">The user's home: MakeMKV's folder and ~ in paths.</param>
/// <param name="SystemKeystore">Whether secrets go to Credential Manager (false in tests: the secrets folder only).</param>
/// <param name="SystemTools">Whether the tools are looked for where they are usually installed and on PATH (false in
/// tests: no tool is found, so nothing runs makemkvcon).</param>
/// <param name="Drives">Whether the optical drives are used (false in tests: no device monitor, no drive control, no MakeMKV
/// drive scan, so nothing asks a drive anything; real hosts pass true).</param>
/// <param name="Environment">The process's environment (the host passes it): ConfigService gets its BROMELIA_SECRET_*
/// entries, the tool locator its PATH.</param>
public sealed record RootOptions(string DataDir, string? ConfigPath, string Home, bool SystemKeystore, bool SystemTools, bool Drives,
    IReadOnlyDictionary<string, string> Environment);
