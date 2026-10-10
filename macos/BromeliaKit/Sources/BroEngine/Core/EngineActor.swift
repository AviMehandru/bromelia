/// The global actor engine code is isolated to (plan §6, §8): services and the core's executor-bound types. The
/// compiler checks the isolation that C# and C assert at run time (EngineExecutor.isCurrent).
@globalActor public actor EngineActor {
    public static let shared = EngineActor()
}
