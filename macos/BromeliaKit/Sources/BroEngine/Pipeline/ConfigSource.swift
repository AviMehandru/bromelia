import BroDomain

/// Where the runner gets the configuration a job runs with (implemented by ConfigService). It is read once, when the
/// job starts, and every step of that run sees the same snapshot (owner, 2026-10-11).
public protocol ConfigSource: Sendable {
    @EngineActor func current() -> Config
}
