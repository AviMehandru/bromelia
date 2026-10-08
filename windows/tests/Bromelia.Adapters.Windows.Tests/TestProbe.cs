using System.Diagnostics;

namespace Bromelia.Adapters.Windows.Tests;

/// <summary>Bromelia.TestProbe, built and copied next to these tests: a tool the launcher tests stop, and the crash
/// points.</summary>
internal static class TestProbe
{
    public static string Dll => Path.Combine(AppContext.BaseDirectory, "Bromelia.TestProbe.dll");

    /// <summary>The dotnet that runs these tests.</summary>
    public static string Dotnet => Environment.GetEnvironmentVariable("DOTNET_HOST_PATH") is { Length: > 0 } host ? host : "dotnet";

    /// <summary>Runs the probe to its end (or its own kill); its exit code.</summary>
    public static int Run(params string[] args)
    {
        var psi = new ProcessStartInfo(Dotnet) { UseShellExecute = false, CreateNoWindow = true };
        psi.ArgumentList.Add(Dll);
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        if (!p.WaitForExit(60_000)) { p.Kill(true); throw new TimeoutException("the probe ran for a minute"); }
        return p.ExitCode;
    }
}
