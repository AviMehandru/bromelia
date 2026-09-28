import Testing
import Foundation
@testable import Bromelia

private final class BundleToken {}

private func fixtureText(_ name: String) throws -> String {
    let url = try #require(Bundle(for: BundleToken.self).url(forResource: name, withExtension: "txt"), "missing fixture \(name)")
    return try String(contentsOf: url, encoding: .utf8)
}

/// shared/fixtures/dvd-play-all: the IFO files (navigation only, no video) of a One Piece DVD whose
/// title 11 plays six episodes back to back.
private let playAllFixture = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().appendingPathComponent("shared/fixtures/dvd-play-all").path

@Suite("Disc labels and identity")
struct IdentityTests {
    @Test(arguments: [
        ("ONE_PIECE_S2_P7_D2", "One Piece", 2, 7, nil as Int?, 2, true),
        ("One_Piece_S3_P1_D1", "One Piece", 3, 1, nil, 1, true),
        ("THE_LORD_OF_THE_RINGS_DISC_2", "The Lord of the Rings", nil, nil, nil, 2, false),
        ("FRIENDS_SEASON_4_DISC_3", "Friends", 4, nil, nil, 3, true),
        ("BREAKING_BAD_S1D2", "Breaking Bad", 1, nil, nil, 2, true),
        ("NARUTO_VOL_12", "Naruto", nil, nil, 12, nil, true),
        ("BLADE_RUNNER_2049", "Blade Runner 2049", nil, nil, nil, nil, false),
        ("ROCKY_II_WS", "Rocky II", nil, nil, nil, nil, false),
        ("SPIDER-MAN_NO_WAY_HOME", "Spider-Man No Way Home", nil, nil, nil, nil, false),
        ("The Matrix - Disc 1", "The Matrix", nil, nil, nil, 1, false),
    ])
    func parsesLabels(label: String, title: String, season: Int?, part: Int?, volume: Int?, disc: Int?, series: Bool) {
        let l = LabelParser.parse(label)
        #expect(l.title == title)
        #expect(l.season == season)
        #expect(l.part == part)
        #expect(l.volume == volume)
        #expect(l.disc == disc)
        #expect(l.looksLikeSeries == series)
    }

    @Test func describesSet() {
        #expect(LabelParser.parse("ONE_PIECE_S2_P7_D2").setDescription == "Season 2 Part 7 Disc 2")
        #expect(LabelParser.parse("INCEPTION").setDescription == "")
    }

    @Test func resolvesDVDFixture() throws {
        let info = DiscInfoBuilder.build(fromOutput: try fixtureText("info-dvd"))
        let id = MediaIdentity.resolve(info: info, discLabel: "", encrypted: false)
        #expect(id.name == "One Piece")
        #expect(id.kind == .tv)
        #expect(id.format == .dvd)
        #expect(id.formatCode == "DVD")
        #expect(id.label.setDescription == "Season 3 Part 1 Disc 1")
        #expect(MediaIdentity.resolve(info: info, discLabel: "", encrypted: true).formatCode == "DVDe")
        #expect(MediaIdentity.resolve(info: info, discLabel: "", encrypted: false, nameOverride: "One Piece (2001)", kindOverride: .movie).name == "One Piece (2001)")
    }

    @Test func prefersBlurayMetadataTitle() {
        var info = DiscInfo()
        info.attributes[AttributeID.type.rawValue] = "Blu-ray disc"
        info.attributes[AttributeID.name.rawValue] = "The Dark Knight™"
        info.attributes[AttributeID.volumeName.rawValue] = "DARK_KNIGHT_D1"
        let id = MediaIdentity.resolve(info: info, discLabel: "", encrypted: false)
        #expect(id.name == "The Dark Knight")
        #expect(id.kind == .movie)
        #expect(id.formatCode == "BR")
        #expect(id.label.disc == 1)
    }

    @Test func detectsFormats() {
        var uhd = DiscInfo()
        uhd.attributes[AttributeID.type.rawValue] = "Blu-ray disc"
        var t = TitleInfo(index: 0)
        var v = TrackInfo(index: 0)
        v.attributes[AttributeID.type.rawValue] = "Video"
        v.attributes[AttributeID.videoSize.rawValue] = "3840x2160"
        t.tracks = [v]
        uhd.titles = [t]
        #expect(DiscFormat.detect(info: uhd) == .uhd)
        #expect(DiscFormat.uhd.code(encrypted: true) == "4Ke")
        #expect(DiscFormat.detect(info: nil, flags: [.blurayFiles, .aacsFiles]) == .bluray)
        #expect(DiscFormat.detect(info: nil, flags: [.dvdFiles]) == .dvd)
        #expect(DiscFormat.detect(info: nil) == .unknown)
    }

    @Test func detectsUHDBackupFolder() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-bdmv-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: dir) }
        try FileManager.default.createDirectory(at: dir.appendingPathComponent("BDMV"), withIntermediateDirectories: true)
        try Data("INDX0300xxxxxxxx".utf8).write(to: dir.appendingPathComponent("BDMV/index.bdmv"))
        #expect(DiscFormat.detect(backupFolder: dir) == .uhd)
        try Data("INDX0200xxxxxxxx".utf8).write(to: dir.appendingPathComponent("BDMV/index.bdmv"))
        #expect(DiscFormat.detect(backupFolder: dir) == .bluray)
    }

    @Test func tvShowFromEpisodeLengthTitles() {
        var info = DiscInfo()
        info.attributes[AttributeID.type.rawValue] = "Blu-ray disc"
        info.attributes[AttributeID.volumeName.rawValue] = "SOME_SHOW"
        info.titles = [2710, 2650, 2690, 2705, 300].enumerated().map { i, d in
            var t = TitleInfo(index: i)
            t.attributes[AttributeID.duration.rawValue] = TitleInfo.formatDuration(d)
            return t
        }
        #expect(MediaIdentity.episodeLikeTitles(info.titles).map(\.index) == [0, 1, 2, 3])
        #expect(MediaIdentity.resolve(info: info, discLabel: "", encrypted: false).kind == .tv)
    }

    @Test func trackLabels() {
        var dvd = TitleInfo(index: 0)
        dvd.attributes[AttributeID.originalTitleId.rawValue] = "11"
        #expect(MediaIdentity.trackLabel(dvd) == "Title 11")
        var bd = TitleInfo(index: 3)
        bd.attributes[AttributeID.sourceFileName.rawValue] = "00800.mpls"
        #expect(MediaIdentity.trackLabel(bd) == "Playlist 00800")
        #expect(MediaIdentity.episodeLabel(7, width: 3) == "Episode 007")
    }
}

@Suite("Naming and plugins")
struct NamingTests {
    func render(_ extra: [String: String]) -> String {
        let id = MediaIdentity(name: "One Piece", kind: .tv, format: .dvd, encrypted: false,
                               label: LabelParser.parse("ONE_PIECE_S2_P7_D2"), reason: "")
        var v = id.templateValues(rip: "Rip")
        for (k, x) in extra { v[k] = x }
        return TemplateRenderer.renderPath(OutputConfig.defaultFileNameTemplate, values: v)
    }

    @Test func defaultTemplate() {
        #expect(render(["episode": "Episode 138", "track": "Title 11 Ch 1-7"])
                == "One Piece - Episode 138 - Season 2 Part 7 Disc 2 - Rip - Title 11 Ch 1-7 - DVD")
        #expect(render(["rip": "Backup", "format": "DVDe"]) == "One Piece - Season 2 Part 7 Disc 2 - Backup - DVDe")
        let movie = MediaIdentity(name: "Inception", kind: .movie, format: .uhd, encrypted: false, label: LabelInfo(), reason: "")
        var v = movie.templateValues(rip: "Rip")
        v["track"] = "Playlist 00800"
        #expect(TemplateRenderer.renderPath(OutputConfig.defaultFileNameTemplate, values: v) == "Inception - Rip - Playlist 00800 - 4K")
        #expect(TemplateRenderer.renderPath(OutputConfig.defaultFolderTemplate, values: v) == "Inception")
    }

    @Test func pluginMatching() {
        var step = PostProcessStep()
        #expect(PluginMatcher.matches(step, name: "Anything", discLabel: "X", formatCode: "BR"))
        step.matchName = "^one piece$"
        #expect(PluginMatcher.matches(step, name: "One Piece", discLabel: "ONE_PIECE_S2", formatCode: "DVD"))
        #expect(!PluginMatcher.matches(step, name: "Naruto", discLabel: "NARUTO", formatCode: "DVD"))
        step.matchName = "S2_P7"   // specific disc, by label
        #expect(PluginMatcher.matches(step, name: "One Piece", discLabel: "ONE_PIECE_S2_P7_D2", formatCode: "DVD"))
        step.matchName = ""
        step.matchFormats = ["BR*", "4K"]
        #expect(PluginMatcher.matches(step, name: "", discLabel: "", formatCode: "BRe"))
        #expect(PluginMatcher.matches(step, name: "", discLabel: "", formatCode: "4K"))
        #expect(!PluginMatcher.matches(step, name: "", discLabel: "", formatCode: "4Ke"))
        #expect(!PluginMatcher.matches(step, name: "", discLabel: "", formatCode: "DVD"))
        step.matchName = "("
        #expect(!PluginMatcher.matches(step, name: "x", discLabel: "", formatCode: "BR"))
        #expect(PluginMatcher.validate("(") != nil)
    }

    @Test func upgradesVersion1Naming() throws {
        let json = #"{"version":1,"defaultDrive":{"output":{"folderTemplate":"{disc}","fileNameTemplate":""}},"#
            + #""drives":[{"name":"A","output":{"folderTemplate":"{type}/{disc}","fileNameTemplate":"{disc} {n}"}}],"#
            + #""plugins":[{"name":"P","matchName":"piece","matchFormats":["DVD"]}]}"#
        let c = try JSONDecoder().decode(AppConfig.self, from: Data(json.utf8))
        #expect(c.version == 2)
        #expect(c.defaultDrive.output.folderTemplate == OutputConfig.defaultFolderTemplate)
        #expect(c.defaultDrive.output.fileNameTemplate == OutputConfig.defaultFileNameTemplate)
        #expect(c.drives[0].output.folderTemplate == "{type}/{disc}")
        #expect(c.drives[0].output.fileNameTemplate == "{disc} {n}")
        #expect(c.plugins.first?.matchFormats == ["DVD"])
        #expect(c.defaultDrive.archive.checksums)
        #expect(c.defaultDrive.episodes.splitPlayAll)
        // Version 2 files keep an explicitly empty template (MakeMKV's names).
        let v2 = try JSONDecoder().decode(AppConfig.self, from: Data(#"{"version":2,"defaultDrive":{"output":{"fileNameTemplate":""}}}"#.utf8))
        #expect(v2.defaultDrive.output.fileNameTemplate == "")
    }
}

@Suite("Checksums")
struct ChecksumTests {
    @Test func hashesFilesAndFolders() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("bromelia-sums-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: dir) }
        try FileManager.default.createDirectory(at: dir.appendingPathComponent("backup/VIDEO_TS"), withIntermediateDirectories: true)
        try Data("abc".utf8).write(to: dir.appendingPathComponent("a.mkv"))
        try Data().write(to: dir.appendingPathComponent("backup/VIDEO_TS/VIDEO_TS.IFO"))
        let files = Checksums.files([dir.appendingPathComponent("a.mkv"), dir.appendingPathComponent("backup")], base: dir)
        #expect(files.map(\.relative) == ["a.mkv", "backup/VIDEO_TS/VIDEO_TS.IFO"])
        let abc = try Checksums.sha256(of: dir.appendingPathComponent("a.mkv"))
        #expect(abc == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        let entries = try files.map { Checksums.Entry(path: $0.relative, size: 0, sha256: try Checksums.sha256(of: $0.url)) }
        try Checksums.render(entries).write(to: dir.appendingPathComponent(Checksums.fileName), atomically: true, encoding: .utf8)
        #expect(Checksums.parse(Checksums.render(entries)).map(\.path) == ["a.mkv", "backup/VIDEO_TS/VIDEO_TS.IFO"])
        #expect(try Checksums.verify(folder: dir).isEmpty)
        try Data("abd".utf8).write(to: dir.appendingPathComponent("a.mkv"))
        #expect(try Checksums.verify(folder: dir) == ["a.mkv"])
    }
}

@Suite("DVD navigation")
struct DVDNavigationTests {
    @Test func findsPlayAllEpisodes() throws {
        let reader = try #require(FolderVideoTS(path: playAllFixture, label: "ONE_PIECE_S2_P7_D2"))
        let a = try #require(DVDNavigation.analyse(reader))
        let plan = try #require(DVDNavigation.plans(a).first)
        #expect(plan.title == 11)
        #expect(plan.starts == [1, 8, 15, 22, 29, 36])
        #expect(plan.lastEnd == 42)
        #expect(plan.endRule == "VOB boundary")
        #expect(plan.tail == [43])
        #expect(plan.splitChapters == [8, 15, 22, 29, 36, 43])
        #expect(DVDNavigation.hms(plan.duration) == "2:21:51.370")
        #expect(DVDNavigation.hms(plan.chapterStarts[7]) == "0:23:36.815")
        #expect(plan.chapterRange(0) == 1...7)
        #expect(plan.chapterRange(5) == 36...42)
        #expect(plan.isPlausible(strict: true))
        #expect(plan.reasons[1] == "if GPRM7 == 21: LinkPTTN 8")
    }

    @Test func decodesJumps() {
        // JumpVTS_PTT 3:5 and LinkPTTN 8 with a condition.
        #expect(DVDNavigation.decodeJump(Data([0x30, 0x05, 0x00, 0x05, 0x00, 0x03, 0x00, 0x00])) == .ptt(titleNumber: 3, ptt: 5, condition: ""))
        #expect(DVDNavigation.decodeJump(Data([0x20, 0xA5, 0x00, 0x07, 0x00, 0x15, 0x00, 0x08])) == .ptt(titleNumber: nil, ptt: 8, condition: "if GPRM7 == 21: "))
        #expect(DVDNavigation.decodeJump(Data([0x30, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00])) == .title(4, condition: ""))
        #expect(DVDNavigation.decodeJump(Data(repeating: 0, count: 8)) == nil)
    }

    @Test func picksFirstEpisode() {
        #expect(DVDNavigation.firstEpisode(from: [138, 139, 140, 142, 143], count: 6) == 138)
        #expect(DVDNavigation.firstEpisode(from: [5], count: 6) == nil)
        #expect(DVDNavigation.episodeNumbers(inText: "EPISODE 138\nEpis0de #12 foo episode9") == [138, 12, 9])
    }

    @Test func mapsChaptersToMKV() throws {
        let reader = try #require(FolderVideoTS(path: playAllFixture))
        let analysis = try #require(DVDNavigation.analyse(reader))
        let plan = try #require(DVDNavigation.plans(analysis).first)
        // An MKV with an extra chapter 00 at the start: every disc chapter shifts by one.
        let shifted = [0.0] + plan.chapterStarts.map { $0 + 0.02 }
        #expect(DVDNavigation.mkvChapters(for: plan.splitChapters, plan: plan, mkvStarts: shifted) == [9, 16, 23, 30, 37, 44])
        #expect(DVDNavigation.mkvChapters(for: [8], plan: plan, mkvStarts: [0, 10, 20]) == nil)
        #expect(DVDNavigation.mkvChapters(for: [8, 15], plan: plan, mkvStarts: nil) == [8, 15])
        #expect(EpisodeSplitter.parseSimpleChapters("CHAPTER01=00:00:00.000\nCHAPTER01NAME=x\nCHAPTER02=00:23:36.815\n") == [0, 1416.815])
    }

    /// The same analysis straight from a DVD ISO (TEST_RUNNER_BROMELIA_TEST_DVD_ISO=/path/One_Piece_S2_P7_D2.iso).
    @Test(.enabled(if: ProcessInfo.processInfo.environment["BROMELIA_TEST_DVD_ISO"] != nil))
    func readsISO() async throws {
        let reader = try #require(ISOVideoTS(path: ProcessInfo.processInfo.environment["BROMELIA_TEST_DVD_ISO"]!))
        #expect(reader.label == "ONE_PIECE_S2_P7_D2")
        let analysis = try #require(DVDNavigation.analyse(reader))
        let plan = try #require(DVDNavigation.plans(analysis).first)
        #expect(plan.starts == [1, 8, 15, 22, 29, 36])
        let (numbers, why) = await EpisodeSplitter.ocrEpisodeNumbers(reader, isCancelled: { false })
        if let numbers {
            #expect(DVDNavigation.firstEpisode(from: numbers, count: plan.episodeCount) == 138)
        } else {
            print("OCR skipped: \(why ?? "")")
        }
    }
}
