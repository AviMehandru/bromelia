import BroDomain
import BroFoundation

/// Starts processes, never through a shell.
public protocol ProcessLauncher: Sendable {
    /// Starts the process; fails when it can't be started.
    func start(_ spec: ProcessSpec) throws(BroError) -> any RunningProcess
}
