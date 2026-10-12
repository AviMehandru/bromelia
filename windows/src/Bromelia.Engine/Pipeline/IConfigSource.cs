using Bromelia.Domain;

namespace Bromelia.Engine;

/// <summary>Where the runner gets the configuration a job runs with (implemented by ConfigService). It is read once,
/// when the job starts, and every step of that run sees the same snapshot (owner, 2026-10-11).</summary>
public interface IConfigSource
{
    Config Current();
}
