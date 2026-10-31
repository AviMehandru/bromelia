using System.Collections.Generic;

namespace Bromeliad;

/// <summary>What the graph is built for. Phase 3 has no defaults (HostOptions gives them in phase 4): the platform's
/// data folder is today's app's, with the real config.json and secrets, so a test always passes its own.</summary>
/// <param name="DataDir">The data folder: bromelia.db, jobs, secrets, logs (made when it isn't there).</param>
/// <param name="ConfigPath">config.json; null: &lt;data&gt;/config.json.</param>
/// <param name="Home">The user's home: MakeMKV's folder and ~ in paths.</param>
/// <param name="SystemKeystore">Whether secrets go to Credential Manager (false in tests: the secrets folder only).</param>
/// <param name="SystemTools">Whether the tools are looked for where they are usually installed and on PATH (false in
/// tests: no tool is found, so nothing runs makemkvcon).</param>
/// <param name="Environment">The process's environment (the host passes it): ConfigService gets its BROMELIA_SECRET_*
/// entries, the tool locator its PATH.</param>
public sealed record RootOptions(string DataDir, string? ConfigPath, string Home, bool SystemKeystore, bool SystemTools,
    IReadOnlyDictionary<string, string> Environment);
