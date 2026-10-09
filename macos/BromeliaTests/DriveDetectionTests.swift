import Testing
import Foundation
@testable import Bromelia

/// Drive scan handling: disc insertion, automatic rips, configuration matching.
@Suite("Drive detection", .serialized)
@MainActor
struct DriveDetectionTests {
    init() {
        Paths.dataOverride = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-test-data", isDirectory: true)
    }

    func entry(_ index: Int, _ state: DriveState, name: String, dev: String, disc: String = "") -> DriveScanEntry {
        DriveScanEntry(index: index, state: state, flags: state == .inserted ? [.blurayFiles] : [], driveName: name, discName: disc, devicePath: dev)
    }

    @Test func autoRipQueuesOnInsertOnly() {
        let model = AppModel(persistent: false)
        var left = DriveConfig()
        left.name = "Left"
        left.match = DriveMatch(driveName: "BD-RE TEST DRIVE 1.00 SERIAL1")
        left.automation.autoRipOnInsert = true
        left.automation.autoRipDelaySeconds = 60
        left.rip.mode = .backupThenMkv
        model.config.drives = [left]

        let emptyLeft = entry(0, .emptyClosed, name: "BD-RE  TEST DRIVE 1.00 SERIAL1", dev: "/dev/rdisk9")
        let emptyRight = entry(1, .emptyClosed, name: "DVD OTHER 2.00", dev: "/dev/rdisk10")
        // A disc already present at the first scan does not start a rip.
        model.debugApplyScan([entry(0, .inserted, name: emptyLeft.driveName, dev: "/dev/rdisk9", disc: "OLD"), emptyRight])
        #expect(model.jobs.isEmpty)
        model.debugApplyScan([emptyLeft, emptyRight])
        #expect(model.jobs.isEmpty)

        // Inserting into the configured drive queues a delayed automatic job.
        let inserted = entry(0, .inserted, name: emptyLeft.driveName, dev: "/dev/rdisk9", disc: "MOVIE")
        model.debugApplyScan([inserted, emptyRight])
        #expect(model.jobs.count == 1)
        let job = try! #require(model.jobs.first)
        #expect(job.state == .waiting)
        #expect(job.isAutomatic)
        #expect(job.mode == .backupThenMkv)
        #expect(job.drive.name == "Left")
        #expect(job.laneKey == "dev:/dev/rdisk9")
        #expect(job.discLabel == "MOVIE")

        // Rescans while the disc stays in do not queue duplicates.
        model.debugApplyScan([inserted, emptyRight])
        #expect(model.jobs.count == 1)

        // A drive without auto-rip does nothing.
        model.debugApplyScan([inserted, entry(1, .inserted, name: "DVD OTHER 2.00", dev: "/dev/rdisk10", disc: "X")])
        #expect(model.jobs.count == 1)

        // Sidebar items: the configured drive and the unconfigured one.
        let items = model.driveItems
        #expect(items.count == 2)
        #expect(items.first { $0.entry?.devicePath == "/dev/rdisk9" }?.config?.name == "Left")
        #expect(items.first { $0.entry?.devicePath == "/dev/rdisk10" }?.config == nil)

        // Cancelling a waiting job finishes it without running anything.
        model.cancel(job)
        #expect(job.state == .cancelled)
        #expect(model.history.first?.id == job.id)
    }

    @Test func reinsertedDiscStartsARipWhenTheEmptyDriveHadNoDevicePath() {
        // As MakeMKV lists a drive on macOS: no device path while it is empty, then often the same /dev/rdiskN again.
        let model = AppModel(persistent: false)
        model.config.defaultDrive.automation.autoRipOnInsert = true
        model.config.defaultDrive.automation.autoRipDelaySeconds = 60
        let name = "BD-RE TEST DRIVE 1.00 SERIAL1"
        model.debugApplyScan([entry(0, .inserted, name: name, dev: "/dev/rdisk12", disc: "FIRST")])
        model.debugApplyScan([entry(0, .emptyOpen, name: name, dev: "")])
        model.debugApplyScan([entry(0, .loading, name: name, dev: "")])
        #expect(model.jobs.isEmpty)
        model.debugApplyScan([entry(0, .inserted, name: name, dev: "/dev/rdisk12", disc: "SECOND")])
        #expect(model.jobs.count == 1)
        #expect(model.jobs.first?.discLabel == "SECOND")
        if let job = model.jobs.first { model.cancel(job) }
    }

    @Test func disconnectedConfigurationsStayVisibleAndSetUpCopiesDefaults() {
        let model = AppModel(persistent: false)
        model.config.defaultDrive.rip.titleSelection.strategy = .longest
        model.config.defaultDrive.postProcess = [PostProcessStep()]
        let e = entry(2, .emptyOpen, name: "BD-RE NEW DRIVE 3.00 SN", dev: "/dev/rdisk11")
        model.debugApplyScan([e])
        let config = model.configure(e)
        #expect(config.name == "NEW DRIVE 3.00")
        #expect(config.rip.titleSelection.strategy == .longest)
        #expect(config.id != model.config.defaultDrive.id)
        #expect(config.postProcess.first?.id != model.config.defaultDrive.postProcess.first?.id)
        #expect(model.configure(e).id == config.id)
        model.debugApplyScan([])
        let items = model.driveItems
        #expect(items.count == 1)
        #expect(items.first?.isConnected == false)
        #expect(items.first?.config?.id == config.id)
    }
}
