import Testing
import Foundation
@testable import Bromelia

private final class BundleToken {}

private func fixture(_ name: String) throws -> String {
    let bundle = Bundle(for: BundleToken.self)
    let url = try #require(bundle.url(forResource: name, withExtension: "txt"), "missing fixture \(name)")
    return try String(contentsOf: url, encoding: .utf8)
}

@Suite("Robot protocol")
struct RobotParserTests {
    @Test func splitsQuotedFields() {
        let f = RobotParser.splitFields(#"1,2,"a, b","say \"hi\"","back\\slash",plain"#)
        #expect(f == ["1", "2", "a, b", #"say "hi""#, #"back\slash"#, "plain"])
    }

    @Test func parsesMessage() throws {
        let ev = RobotParser.parse(line: #"MSG:5036,260,1,"Copy complete. 1 titles saved.","Copy complete. %1 titles saved.","1""#)
        guard case .message(let m)? = ev else { Issue.record("not a message"); return }
        #expect(m.code == 5036)
        #expect(m.flags == 260)
        #expect(m.parameters == ["1"])
        #expect(m.severity == .info)
    }

    @Test func classifiesSeverity() {
        let debug = RobotMessage(code: 1003, flags: 16777248, text: "DEBUG: Code 0", format: "", parameters: [])
        let err = RobotMessage(code: 5037, flags: 516, text: "Copy complete. 0 titles saved, 1 failed.", format: "", parameters: ["0", "1"])
        let warn = RobotMessage(code: 1, flags: 1028, text: "x", format: "", parameters: [])
        #expect(debug.severity == .debug)
        #expect(err.severity == .error)
        #expect(warn.severity == .warning)
    }

    @Test func parsesProgress() {
        #expect(RobotParser.parse(line: "PRGV:5957,356,65536") == .progressValue(current: 5957, total: 356, max: 65536))
        #expect(RobotParser.parse(line: #"PRGT:5018,0,"Scanning CD-ROM devices""#) == .progressTotalTitle(code: 5018, id: 0, name: "Scanning CD-ROM devices"))
        #expect(RobotParser.parse(line: #"PRGC:3102,0,"Processing title sets""#) == .progressCurrentTitle(code: 3102, id: 0, name: "Processing title sets"))
        #expect(RobotParser.parse(line: "TCOUNT:3") == .titleCount(3))
        #expect(RobotParser.parse(line: "Backup source must start with \"disc:\"") == .raw("Backup source must start with \"disc:\""))
        #expect(RobotParser.parse(line: "") == nil)
    }

    @Test func parsesDriveScanFixture() throws {
        let text = try fixture("drive-scan")
        var drives: [DriveScanEntry] = []
        text.enumerateLines { l, _ in if case .drive(let d)? = RobotParser.parse(line: l) { drives.append(d) } }
        #expect(drives.count == 8)
        let present = drives.filter(\.isPresent)
        #expect(present.count == 5)
        #expect(present[0].state == .inserted)
        #expect(present[0].flags.contains(.blurayFiles))
        #expect(present[0].flags.contains(.aacsFiles))
        #expect(present[0].flags.discTypeName == "Blu-ray (AACS)")
        #expect(present[0].discName == "MOVIE_DISC")
        #expect(present[0].devicePath == "/dev/rdisk4")
        #expect(present[1].state == .emptyClosed)
        #expect(present[2].state == .emptyOpen)
        #expect(present[3].state == .loading)
        #expect(present[4].driveName == #"DVD+RW Some "Quoted" Drive"#)
        #expect(present[4].discName == "Disc, With Comma")
    }

    @Test func buildsDiscInfoFromRealDVD() throws {
        let info = DiscInfoBuilder.build(fromOutput: try fixture("info-dvd"))
        #expect(info.reportedTitleCount == 3)
        #expect(info.titles.count == 3)
        #expect(info.typeName == "DVD disc")
        #expect(info.typeToken == "dvd")
        #expect(info.name == "One_Piece_S3_P1_D1")
        let t0 = try #require(info.title(at: 0))
        #expect(t0.chapterCount == 50)
        #expect(t0.durationText == "2:44:48")
        #expect(t0.durationSeconds == 2 * 3600 + 44 * 60 + 48)
        #expect(t0.sizeBytes == 8_075_685_888)
        #expect(t0.sourceTitleId == 11)
        #expect(t0.outputFileName == "B1_t00.mkv")
        #expect(t0.segmentMap == "1-7,8-14,15-21,22-28,29-35,36-42,43-49,50")
        let t1 = try #require(info.title(at: 1))
        #expect(t1.tracks.count == 6)
        #expect(t1.tracks[0].kind == .video)
        #expect(t1.tracks[1].kind == .audio)
        #expect(t1.tracks[1].languageCode == "eng")
        #expect(t1.tracks[1].isDefault)
        #expect(t1.tracks[3].languageCode == "jpn")
        #expect(t1.tracks[5].kind == .subtitle)
    }

    @Test func ripFixtureReportsSuccess() throws {
        var saved: Int?
        try fixture("mkv-rip").enumerateLines { l, _ in
            if case .message(let m)? = RobotParser.parse(line: l), m.code == 5036 { saved = Int(m.parameters[0]) }
        }
        #expect(saved == 1)
    }

    @Test func failureFixtureReportsErrors() throws {
        var errors: [RobotMessage] = []
        try fixture("rip-failure").enumerateLines { l, _ in
            if case .message(let m)? = RobotParser.parse(line: l), m.severity == .error { errors.append(m) }
        }
        #expect(errors.map(\.code) == [2003, 5003, 5037])
        #expect(errors.last?.parameters == ["0", "1"])
    }
}

@Suite("Title selection")
struct TitleSelectorTests {
    func title(_ i: Int, _ dur: String, chapters: Int = 10, size: Int64 = 1_000_000_000, src: Int? = nil, segments: String = "", name: String = "", file: String = "") -> TitleInfo {
        var t = TitleInfo(index: i)
        t.attributes[AttributeID.duration.rawValue] = dur
        t.attributes[AttributeID.chapterCount.rawValue] = String(chapters)
        t.attributes[AttributeID.diskSizeBytes.rawValue] = String(size)
        if let src { t.attributes[AttributeID.originalTitleId.rawValue] = String(src) }
        if !segments.isEmpty { t.attributes[AttributeID.segmentsMap.rawValue] = segments }
        if !name.isEmpty { t.attributes[AttributeID.name.rawValue] = name }
        if !file.isEmpty { t.attributes[AttributeID.sourceFileName.rawValue] = file }
        return t
    }

    var titles: [TitleInfo] {
        [title(0, "1:58:02", chapters: 24, size: 30_000_000_000, src: 800, segments: "1,2,3", file: "00800.mpls"),
         title(1, "1:58:02", chapters: 24, size: 30_000_000_000, src: 801, segments: "1,2,3", file: "00801.mpls"),
         title(2, "0:22:10", chapters: 5, size: 2_000_000_000, src: 10, file: "00010.mpls"),
         title(3, "0:23:15", chapters: 6, size: 2_100_000_000, src: 11, file: "00011.mpls"),
         title(4, "0:03:00", chapters: 1, size: 100_000_000, src: 20, name: "Trailer", file: "00020.mpls")]
    }

    @Test func allWithDuplicateSkipping() {
        var r = TitleSelection()
        r.strategy = .all
        let res = TitleSelector.evaluate(titles, rule: r)
        #expect(res.selectedIndices == [0, 2, 3, 4])
        #expect(res.decisions.first { $0.titleIndex == 1 }?.reason == "Duplicate of title 0")
        r.skipDuplicates = false
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [0, 1, 2, 3, 4])
    }

    @Test func mainFeature() {
        var r = TitleSelection()
        r.strategy = .longest
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [0])
        r.longestCount = 3
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [0, 2, 3])
    }

    @Test func episodesByDuration() {
        var r = TitleSelection()
        r.minDurationSeconds = 20 * 60
        r.maxDurationSeconds = 30 * 60
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [2, 3])
    }

    @Test func indexPatterns() {
        var r = TitleSelection()
        r.strategy = .indices
        r.skipDuplicates = false
        r.indexPattern = "0, 3-"
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [0, 3, 4])
        r.indexPattern = "last"
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [4])
        r.indexBase = .source
        r.indexPattern = "800-801,11"
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [0, 1, 3])
        r.indexPattern = "x"
        #expect(TitleSelector.evaluate(titles, rule: r).error != nil)
    }

    @Test func regexFilters() {
        var r = TitleSelection()
        r.skipDuplicates = false
        r.includePattern = "0080[01]\\.mpls"
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [0, 1])
        r.includePattern = ""
        r.excludePattern = "trailer"
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [0, 1, 2, 3])
        r.excludePattern = "#10\\b"
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [0, 1, 3, 4])
    }

    @Test func limitsAndManual() {
        var r = TitleSelection()
        r.maxTitles = 2
        #expect(TitleSelector.evaluate(titles, rule: r).selectedIndices == [0, 2])
        r.strategy = .manual
        let res = TitleSelector.evaluate(titles, rule: r)
        #expect(res.requiresManualChoice)
        #expect(res.selectedIndices.isEmpty)
    }

    @Test func worksOnRealDisc() throws {
        let info = DiscInfoBuilder.build(fromOutput: try fixture("info-dvd"))
        var r = TitleSelection()
        r.strategy = .longest
        #expect(TitleSelector.evaluate(info.titles, rule: r).selectedIndices == [0])
    }
}

@Suite("Templates and arguments")
struct TemplateTests {
    @Test func rendersTokens() {
        let v = ["disc": "MOVIE", "n": "3", "comment": "", "title": "A/B: C"]
        #expect(TemplateRenderer.render("{disc} - {n:2}", values: v) == "MOVIE - 03")
        #expect(TemplateRenderer.render("{disc}{comment? ({comment})}", values: v) == "MOVIE")
        #expect(TemplateRenderer.render("{disc}{n? #{n}}", values: v) == "MOVIE #3")
        #expect(TemplateRenderer.render("{unknown}", values: v) == "{unknown}")
        #expect(TemplateRenderer.renderPath("Movies/{title}", values: v) == "Movies/A-B- C")
        #expect(TemplateRenderer.renderPath("../{disc}/./", values: v) == "MOVIE")
    }

    @Test func sanitizes() {
        #expect(TemplateRenderer.sanitizeComponent("a:b/c\\d*e?f\"g<h>i|j") == "a-b-c-d-e-f-g-h-i-j")
        #expect(TemplateRenderer.sanitizeComponent("  .name. ") == "name")
    }

    @Test func splitsArguments() {
        #expect(ArgumentSplitter.split(#"-i "my file.mkv" --x='a b' c\ d"#) == ["-i", "my file.mkv", "--x=a b", "c d"])
        #expect(ArgumentSplitter.split(#""" x"#) == ["", "x"])
        #expect(ArgumentSplitter.split("  ") == [])
        #expect(ArgumentSplitter.quote("simple") == "simple")
        #expect(ArgumentSplitter.quote("it's") == #"'it'\''s'"#)
    }

    @Test func postProcessInvocation() {
        var step = PostProcessStep()
        step.executable = "/bin/echo"
        step.arguments = #"--dir {outputDir} {files} "{disc} done""#
        let (exe, args) = PostProcessor.buildInvocation(step, values: ["outputDir": "/out/My Disc", "disc": "My Disc"],
                                                         files: [URL(fileURLWithPath: "/out/a.mkv"), URL(fileURLWithPath: "/out/b c.mkv")])
        #expect(exe.path == "/bin/echo")
        #expect(args == ["--dir", "/out/My Disc", "/out/a.mkv", "/out/b c.mkv", "My Disc done"])
        step.interpreter = "/usr/bin/python3"
        step.executable = "~/x.py"
        let (exe2, args2) = PostProcessor.buildInvocation(step, values: [:], files: [])
        #expect(exe2.path == "/usr/bin/python3")
        #expect(args2.first == NSString(string: "~/x.py").expandingTildeInPath)
    }

    @Test func runConditions() {
        var s = PostProcessStep()
        s.executable = "/bin/true"
        #expect(PostProcessor.shouldRun(s, status: .succeeded))
        #expect(!PostProcessor.shouldRun(s, status: .failed))
        s.runOn = .failure
        #expect(PostProcessor.shouldRun(s, status: .failed))
        #expect(PostProcessor.shouldRun(s, status: .cancelled))
        s.runOn = .always
        s.enabled = false
        #expect(!PostProcessor.shouldRun(s, status: .succeeded))
    }
}

@Suite("MakeMKV files")
struct MakeMKVFileTests {
    @Test func settingsRoundTrip() {
        let text = """
        #
        # MakeMKV settings file
        #
        app_DestinationDir = "/Users/me/Movies"
        app_ExpertMode = "1"
        dvd_MinimumTitleLength = "120"
        """
        let s = SettingsConf.parse(text)
        #expect(s == ["app_DestinationDir": "/Users/me/Movies", "app_ExpertMode": "1", "dvd_MinimumTitleLength": "120"])
        #expect(SettingsConf.parse(SettingsConf.serialize(s)) == s)
    }

    @Test func profileIsWellFormedXML() throws {
        var p = GeneratedProfile()
        p.name = "Test & <Profile>"
        p.selectionRule = "-sel:all,+sel:(eng|jpn)&audio"
        let xml = ProfileBuilder.build(p)
        let parser = XMLParser(data: Data(xml.utf8))
        #expect(parser.parse())
        #expect(xml.contains("app_DefaultSelectionString=\"-sel:all,+sel:(eng|jpn)&amp;audio\""))
        #expect(xml.contains("outputSettingsName=\"flac-best\""))
        let def = ProfileBuilder.build(GeneratedProfile())
        #expect(def.contains(ProfileBuilder.makemkvDefaultSelection))
    }

    @Test func environmentIsolatesSettings() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-test-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: home) }
        var cfg = AppConfig()
        cfg.registrationKey = "T-TESTKEY"
        cfg.globalSettings = ["dvd_MinimumTitleLength": "120", "app_ShowDebug": "1", "app_Proxy": ""]
        var drive = DriveConfig()
        drive.settings = ["dvd_MinimumTitleLength": "30"]
        drive.profile.mode = .generated
        drive.rip.minLengthSeconds = 45
        drive.rip.cacheMB = 256
        drive.rip.directIO = false
        let env = try MakeMKVEnvironment.prepare(executable: URL(fileURLWithPath: "/bin/echo"), config: cfg, drive: drive, home: home,
                                                 selectionOverride: "+sel:all")
        let conf = try String(contentsOf: home.appendingPathComponent("Library/MakeMKV/settings.conf"), encoding: .utf8)
        let parsed = SettingsConf.parse(conf)
        #expect(parsed["dvd_MinimumTitleLength"] == "30")
        #expect(parsed["app_ShowDebug"] == "1")
        #expect(parsed["app_Key"] == "T-TESTKEY")
        #expect(parsed["app_Proxy"] == nil)
        #expect(parsed["app_DefaultSelectionString"] == "+sel:all")
        #expect(parsed["app_DataDir"] != nil)
        #expect(env.processEnvironment["HOME"] == home.path)
        let profile = try #require(env.profilePath)
        #expect(FileManager.default.fileExists(atPath: profile))

        let src = DiscSource.drive(index: 2, devicePath: "/dev/rdisk5")
        let info = env.infoArguments(source: src, rip: drive.rip)
        #expect(info.suffix(2) == ["info", "dev:/dev/rdisk5"])
        #expect(info.contains("--minlength=45"))
        #expect(info.contains("--cache=256"))
        #expect(info.contains("--directio=false"))
        #expect(info.contains("--profile=\(profile)"))
        #expect(info.contains("--noscan"))
        let mkv = env.mkvArguments(source: .iso(path: "/a b.iso"), title: "all", destination: "/out", rip: nil)
        #expect(mkv.suffix(4) == ["mkv", "iso:/a b.iso", "all", "/out"])
        #expect(env.backupArguments(source: src, decrypt: true, destination: "/bk", rip: nil)?.suffix(4) == ["backup", "--decrypt", "disc:2", "/bk"])
        #expect(env.backupArguments(source: .folder(path: "/x"), decrypt: false, destination: "/bk", rip: nil) == nil)
    }
}

@Suite("Configuration")
struct ConfigurationTests {
    @Test func decodesPartialJSON() throws {
        let json = """
        { "outputRoot": "/rips", "drives": [ { "name": "Left", "match": { "driveName": "BD-RE  HL-DT-ST" },
          "rip": { "mode": "backupThenMkv", "titleSelection": { "strategy": "longest" } },
          "postProcess": [ { "executable": "/bin/true" } ], "futureField": 1 } ] }
        """
        let cfg = try JSONDecoder().decode(AppConfig.self, from: Data(json.utf8))
        #expect(cfg.outputRoot == "/rips")
        #expect(cfg.pollIntervalSeconds == 10)
        let d = try #require(cfg.drives.first)
        #expect(d.name == "Left")
        #expect(d.rip.mode == .backupThenMkv)
        #expect(d.rip.titleSelection.strategy == .longest)
        #expect(d.rip.titleSelection.skipDuplicates)
        #expect(d.postProcess.first?.runOn == .success)
        #expect(d.automation.ejectWhenDone)
    }

    @Test func roundTrips() throws {
        var cfg = AppConfig()
        var d = DriveConfig()
        d.name = "Right"
        d.settings = ["io_ErrorRetryCount": "5"]
        d.rip.minLengthSeconds = 60
        var step = PostProcessStep()
        step.environment = ["A": "{disc}"]
        d.postProcess = [step]
        cfg.drives = [d]
        cfg.presets = [DrivePreset(name: "P", config: d)]
        let data = try JSONEncoder().encode(cfg)
        let back = try JSONDecoder().decode(AppConfig.self, from: data)
        #expect(back == cfg)
    }

    @Test func matchesDrives() {
        let entry = DriveScanEntry(index: 0, state: .inserted, flags: [], driveName: "BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325", discName: "", devicePath: "/dev/rdisk4")
        #expect(DriveMatch(driveName: "bd-re hl-dt-st bd-re wh16ns60 1.02 klam6e84325").matches(entry))
        #expect(!DriveMatch(driveName: "BD-RE ASUS").matches(entry))
        #expect(DriveMatch(devicePath: "/dev/rdisk4").matches(entry))
        var cfg = AppConfig()
        var a = DriveConfig(); a.match = DriveMatch(devicePath: "/dev/rdisk4")
        cfg.drives = [a]
        #expect(cfg.driveConfig(for: entry)?.id == a.id)
    }

    @Test func effectiveSettingsOverlay() {
        var cfg = AppConfig()
        cfg.globalSettings = ["a": "1", "b": "2"]
        var d = DriveConfig()
        d.settings = ["b": "3", "c": "4"]
        #expect(cfg.effectiveSettings(for: d) == ["a": "1", "b": "3", "c": "4"])
    }
}

@Suite("Remux")
struct RemuxTests {
    @Test func buildsTrackArguments() throws {
        let info = DiscInfoBuilder.build(fromOutput: try fixture("info-dvd"))
        let title = try #require(info.title(at: 1))
        let layout: [Remuxer.TrackLayout] = [.init(id: 0, type: "video"), .init(id: 1, type: "audio"), .init(id: 2, type: "audio"),
                                             .init(id: 3, type: "audio"), .init(id: 4, type: "audio"), .init(id: 5, type: "subtitles")]
        let args = try #require(Remuxer.arguments(layout: layout, title: title, keep: [0, 1, 3], input: URL(fileURLWithPath: "/in.mkv"), output: URL(fileURLWithPath: "/out.mkv")))
        #expect(args == ["-o", "/out.mkv", "--video-tracks", "0", "--audio-tracks", "1,3", "--no-subtitles", "/in.mkv"])
        #expect(Remuxer.arguments(layout: Array(layout.prefix(5)), title: title, keep: [0], input: URL(fileURLWithPath: "/i"), output: URL(fileURLWithPath: "/o")) == nil)
    }
}
