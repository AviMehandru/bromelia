import Foundation
import Observation

/// Runs post-processing steps marked "background" (encoding, uploads) after their job has finished and the disc
/// is out, a few at a time, so the drive is free for the next disc.
@MainActor
@Observable
final class BackgroundQueue {
    enum State: String { case queued, running, done, failed }

    struct Item: Identifiable {
        let id = UUID()
        let work: BackgroundWork
        var state: State = .queued
        var message = ""
    }

    private(set) var items: [Item] = [] {
        didSet { onChange?() }
    }
    var limit = 1
    /// Called whenever an item is added, starts, finishes or is cleared.
    @ObservationIgnored var onChange: (() -> Void)?

    func enqueue(_ work: BackgroundWork) {
        items.append(Item(work: work))
        pump()
    }

    func clearFinished() {
        items.removeAll { $0.state == .done || $0.state == .failed }
    }

    var isBusy: Bool { items.contains { $0.state == .running || $0.state == .queued } }

    private func pump() {
        while items.filter({ $0.state == .running }).count < max(1, limit), let i = items.firstIndex(where: { $0.state == .queued }) {
            items[i].state = .running
            let item = items[i]
            Task { await run(item) }
        }
    }

    private func run(_ item: Item) async {
        let logFile = item.work.logFile
        FileManager.default.createFile(atPath: logFile.path, contents: nil)
        let handle = try? FileHandle(forWritingTo: logFile)
        let lock = NSLock()
        let results = await PostProcessor.run(steps: item.work.steps, context: item.work.context, register: { _ in }, log: { text, _ in
            lock.lock()
            handle?.write(Data((text + "\n").utf8))
            lock.unlock()
        })
        try? handle?.close()
        let failed = results.filter { $0.exitCode != 0 || $0.timedOut }
        if let i = items.firstIndex(where: { $0.id == item.id }) {
            items[i].state = failed.isEmpty ? .done : .failed
            items[i].message = failed.isEmpty ? "\(results.count) step(s) finished" : "“\(failed[0].name)” failed; see \(logFile.path)"
        }
        pump()
    }
}
