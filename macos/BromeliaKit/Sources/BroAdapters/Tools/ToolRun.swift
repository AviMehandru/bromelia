import BroDomain
import BroFoundation
import BroPorts

/// How every tool adapter runs its process: hands each output line to `onLine`, which may return a reason to stop it
/// (acted on once), stops it when the token is cancelled, and always leaves with the process ended. When the calling
/// task is cancelled the lines stop early: the process is then stopped (.cancelled) before it is waited for, rather than
/// left to run with nobody reading it.
enum ToolRun {
    /// `started` is called once the process has started (a timer that stops it, for instance).
    static func run(_ launcher: any ProcessLauncher, _ spec: ProcessSpec, cancel: CancellationToken,
                    started: ((any RunningProcess) -> Void)? = nil,
                    onLine: (OutputLine) -> StopReason?) async throws(BroError) -> ProcessExit {
        let process = try launcher.start(spec)
        let remove = cancel.onCancel { process.stop(.cancelled) }
        defer { remove() }
        started?(process)
        var stopped = false
        for await line in process.lines() {
            if let reason = onLine(line), !stopped {
                stopped = true
                process.stop(reason)
            }
        }
        if Task.isCancelled { process.stop(.cancelled) }
        return await process.wait()
    }
}
