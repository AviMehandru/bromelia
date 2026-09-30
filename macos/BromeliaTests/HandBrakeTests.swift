import Testing
import Foundation
@testable import Bromelia

@Suite("HandBrake steps")
struct HandBrakeTests {
    @Test func arguments() {
        var s = PostProcessStep.handbrake()
        #expect(s.kind == .handbrake && s.background && s.preset == "H.265 MKV 1080p30")
        #expect(PostProcessor.shouldRun(s, status: .succeeded) && !PostProcessor.shouldRun(s, status: .failed))
        s.presetFile = "/p/archive.json"
        s.extraArguments = "--all-subtitles --subtitle-burned=none"
        let values = ["outputDir": "/out/Show", "stem": "Show - S01E01", "name": "Show"]
        let out = HandBrake.output(s, values: values)
        #expect(out.path == "/out/Show/Encoded/Show - S01E01.mkv")
        #expect(HandBrake.arguments(s, input: URL(fileURLWithPath: "/out/Show/Show - S01E01.mkv"), output: out)
                == ["--preset-import-file", "/p/archive.json", "--preset", "H.265 MKV 1080p30", "-i", "/out/Show/Show - S01E01.mkv",
                    "-o", "/out/Show/Encoded/Show - S01E01.mkv", "--all-subtitles", "--subtitle-burned=none"])
        #expect(HandBrake.isSource(URL(fileURLWithPath: "/x/a.MKV")) && !HandBrake.isSource(URL(fileURLWithPath: "/x/a.iso")))
        let f = HandBrake.ProgressFilter()
        let kept = ["Encoding: task 1 of 2, 0.50 % (0.0 fps)", "Encoding: task 1 of 2, 3.00 %", "Encoding: task 1 of 2, 10.01 %",
                    "Encoding: task 1 of 2, 19.99 %", "x264 [info]: done", "Encoding: task 2 of 2, 1.00 %", "Encoding: task 2 of 2, 95.00 %"]
            .filter(f.keep)
        #expect(kept == ["Encoding: task 1 of 2, 0.50 % (0.0 fps)", "Encoding: task 1 of 2, 10.01 %", "x264 [info]: done",
                         "Encoding: task 2 of 2, 1.00 %", "Encoding: task 2 of 2, 95.00 %"])
    }

    /// A stand-in HandBrakeCLI copies the input to the output: each MKV gets an encode, an existing one is kept.
    @Test func encodesEveryMKVWithoutReplacingAnything() async throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-hb-\(UUID().uuidString.prefix(6))")
        defer { try? FileManager.default.removeItem(at: dir) }
        try FileManager.default.createDirectory(at: dir.appendingPathComponent("Encoded"), withIntermediateDirectories: true)
        let tool = dir.appendingPathComponent("HandBrakeCLI")
        try "#!/bin/sh\nwhile [ $# -gt 0 ]; do case $1 in -i) i=$2;; -o) o=$2;; esac; shift; done\necho 'Encoding: task 1 of 1, 50.00 %'\ncp \"$i\" \"$o\"\n"
            .write(to: tool, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: tool.path)
        let a = dir.appendingPathComponent("A.mkv"), b = dir.appendingPathComponent("B.mkv"), iso = dir.appendingPathComponent("C.iso")
        for (u, t) in [(a, "a"), (b, "b"), (iso, "c")] { try Data(t.utf8).write(to: u) }
        try Data("old".utf8).write(to: dir.appendingPathComponent("Encoded/A.mkv"))
        var s = PostProcessStep.handbrake()
        s.executable = tool.path
        let ctx = PostProcessor.Context(status: .succeeded, values: ["outputDir": dir.path], outputDirectory: dir, files: [a, b, iso],
                                        manifestPath: "", environment: [:])
        let log = LineCollector()
        let results = await PostProcessor.run(steps: [s], context: ctx, register: { _ in }, log: { t, _ in log.append(t) })
        #expect(results.count == 2 && results.allSatisfy { $0.exitCode == 0 }, "\(log.all)")
        #expect(try String(contentsOf: dir.appendingPathComponent("Encoded/A.mkv"), encoding: .utf8) == "old")
        #expect(try String(contentsOf: dir.appendingPathComponent("Encoded/A (2).mkv"), encoding: .utf8) == "a")
        #expect(try String(contentsOf: dir.appendingPathComponent("Encoded/B.mkv"), encoding: .utf8) == "b")
        #expect(!FileManager.default.fileExists(atPath: dir.appendingPathComponent("Encoded/C.mkv").path))
        s.executable = dir.appendingPathComponent("missing").path
        let missing = await PostProcessor.run(steps: [s], context: ctx, register: { _ in }, log: { t, _ in log.append(t) })
        #expect(missing.count == 1 && missing[0].exitCode == -1)
    }
}
