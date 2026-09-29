import SwiftUI
import UniformTypeIdentifiers

/// Sheet wrapper: edits a draft copy, saved on “Save”.
struct DriveConfigSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let original: DriveConfig
    let session: DiscSession?
    @State private var draft: DriveConfig
    @State private var presetName = ""
    @State private var askPresetName = false
    @State private var confirmDelete = false

    init(original: DriveConfig, session: DiscSession?) {
        self.original = original
        self.session = session
        _draft = State(initialValue: original)
    }

    var body: some View {
        VStack(spacing: 0) {
            DriveConfigEditor(config: $draft, isDefaultTemplate: original.id == model.config.defaultDrive.id, previewInfo: session?.info)
            Divider()
            HStack {
                Menu("Presets") {
                    Button("Save as Preset…") { presetName = draft.name; askPresetName = true }
                    if !model.config.presets.isEmpty {
                        Divider()
                        ForEach(model.config.presets) { p in
                            Button("Apply “\(p.name)”") { draft = model.applyPreset(p, to: draft) }
                        }
                    }
                    Divider()
                    Button("Archive Everything") { draft.applyArchiveEverything() }
                    Text("Backup then MKV, every track, all checks on")
                    Divider()
                    Button("Copy Settings from Default Configuration") { draft = model.applyPreset(DrivePreset(name: "", config: model.config.defaultDrive), to: draft) }
                }
                .fixedSize()
                if original.id != model.config.defaultDrive.id {
                    Button("Delete Configuration…", role: .destructive) { confirmDelete = true }
                }
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Save") {
                    model.updateDrive(draft)
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
            }
            .padding(12)
        }
        .frame(minWidth: 820, idealWidth: 880, minHeight: 620, idealHeight: 700)
        .alert("Save preset", isPresented: $askPresetName) {
            TextField("Name", text: $presetName)
            Button("Save") { model.savePreset(name: presetName.isEmpty ? draft.name : presetName, from: draft) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Presets store everything except the drive's name and identification, so they can be applied to other drives.")
        }
        .confirmationDialog("Delete the configuration “\(original.name)”?", isPresented: $confirmDelete) {
            Button("Delete", role: .destructive) {
                model.removeDriveConfig(original.id)
                dismiss()
            }
        }
    }
}

struct DriveConfigEditor: View {
    @Binding var config: DriveConfig
    var isDefaultTemplate = false
    var previewInfo: DiscInfo?

    var body: some View {
        TabView {
            GeneralTab(config: $config, isDefaultTemplate: isDefaultTemplate)
                .tabItem { Label("General", systemImage: "opticaldiscdrive") }
            RipTab(config: $config, previewInfo: previewInfo)
                .tabItem { Label("Ripping", systemImage: "film.stack") }
            OutputTab(config: $config)
                .tabItem { Label("Output", systemImage: "folder") }
            MakeMKVSettingsTab(settings: $config.settings, mode: .drive)
                .tabItem { Label("MakeMKV Settings", systemImage: "slider.horizontal.3") }
            ProfileTab(profile: $config.profile)
                .tabItem { Label("Profile", systemImage: "doc.badge.gearshape") }
            PostProcessTab(steps: $config.postProcess, driveName: config.name)
                .tabItem { Label("Post-processing", systemImage: "terminal") }
        }
        .padding(12)
    }
}

// MARK: - General

private struct GeneralTab: View {
    @Environment(AppModel.self) private var model
    @Binding var config: DriveConfig
    let isDefaultTemplate: Bool

    var body: some View {
        Form {
            Section {
                TextField("Name", text: $config.name)
                if !isDefaultTemplate {
                    Toggle("Enabled", isOn: $config.enabled)
                }
            } footer: {
                if isDefaultTemplate {
                    Text("The default configuration is used for drives that have not been set up and for disc images and folders. New drive configurations start as a copy of it.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            if !isDefaultTemplate {
                Section("Drive identification") {
                    TextField("Drive name", text: $config.match.driveName, prompt: Text("e.g. BD-RE HL-DT-ST BD-RE WH16NS60 1.02 KL…"))
                    TextField("Device path", text: $config.match.devicePath, prompt: Text("/dev/rdisk4 (used when the drive name is empty)"))
                    let candidates = model.scannedDrives.filter(\.isPresent)
                    if !candidates.isEmpty {
                        Menu("Use a Connected Drive") {
                            ForEach(candidates, id: \.self) { e in
                                Button("\(e.driveName) — \(e.devicePath)") {
                                    config.match = DriveMatch(driveName: e.driveName, devicePath: e.devicePath)
                                }
                            }
                        }
                        .fixedSize()
                    }
                    Text("The drive name reported by MakeMKV usually includes the serial number, so a configuration follows the drive even when device numbers change.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Section("Automation") {
                Toggle("Rip automatically when a disc is inserted", isOn: $config.automation.autoRipOnInsert)
                if config.automation.autoRipOnInsert {
                    LabeledContent("Start after") {
                        IntField(title: "Delay", value: $config.automation.autoRipDelaySeconds, suffix: "seconds")
                    }
                }
                Toggle("Eject the disc when the job succeeds", isOn: $config.automation.ejectWhenDone)
                Toggle("Eject the disc when the job fails", isOn: $config.automation.ejectOnFailure)
                Toggle("Show a notification when the job finishes", isOn: $config.automation.notify)
                Toggle("Play a sound", isOn: $config.automation.playSound)
                    .disabled(!config.automation.notify)
                LabeledContent("Before an automatic rip, wait for the disc to be mounted for up to") {
                    IntField(title: "Seconds", value: $config.automation.waitForMountSeconds, suffix: "seconds")
                }
                Picker("A disc archived before", selection: $config.automation.alreadyArchived) {
                    ForEach(AlreadyArchived.allCases) { Text($0.label).tag($0) }
                }
                .help("Automatic rips. A disc is recognised by its fingerprint, in the history or a bromelia.json under the output folder. Manual rips ask first.")
            }
            Section {
                Toggle("Rip audio CDs", isOn: $config.other.ripAudioCDs)
                if config.other.ripAudioCDs {
                    TextField("Audio CD command", text: $config.other.audioCommand, prompt: Text("Empty: cyanrip -d {device} -o flac, else abcde"))
                }
                Toggle("Save data discs as ISO images", isOn: $config.other.imageDataDiscs)
            } header: {
                Text("Other discs")
            } footer: {
                Text("For discs without a DVD or Blu-ray structure, when ripped automatically or with Rip. Audio CDs are ripped by cyanrip or abcde (both look up the album in MusicBrainz and write FLAC; install one with Homebrew). Data discs are copied sector by sector with hdiutil.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }
}

// MARK: - Ripping

struct RipTab: View {
    @Binding var config: DriveConfig
    let previewInfo: DiscInfo?

    var body: some View {
        Form {
            Section("What to do with a disc") {
                Picker("Mode", selection: $config.rip.mode) {
                    ForEach(RipMode.videoModes) { Text($0.label).tag($0) }
                }
                if config.rip.mode.makesBackup {
                    Picker("Backup format", selection: $config.rip.backupFormat) {
                        ForEach(BackupFormat.allCases) { Text($0.label).tag($0) }
                    }
                }
                if config.rip.mode == .backupThenMkv {
                    Toggle("Keep the backup after the MKV files are made", isOn: $config.rip.keepBackupAfterMKV)
                }
                Text(modeHelp).font(.caption).foregroundStyle(.secondary)
                ForEach([("dvd", "DVDs"), ("bluray", "Blu-rays"), ("uhd", "4K UHD discs")], id: \.0) { key, label in
                    Picker(label, selection: Binding(get: { config.rip.formatModes[key] }, set: { config.rip.formatModes[key] = $0 })) {
                        Text("Same as above").tag(RipMode?.none)
                        ForEach(RipMode.videoModes) { Text($0.label).tag(RipMode?.some($0)) }
                    }
                }
                Text("Automatic and quick rips can use a different mode for each kind of disc, for example a backup of every Blu-ray and MKV files of DVDs.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section {
                Toggle("Split “play all” titles of TV shows into episodes", isOn: $config.episodes.splitPlayAll)
                Toggle("Keep the unsplit title as well", isOn: $config.episodes.keepPlayAll)
                    .disabled(!config.episodes.splitPlayAll)
                Toggle("Read episode numbers from the disc menus", isOn: $config.episodes.readMenuNumbers)
            } header: {
                Text("TV episodes")
            } footer: {
                Text("DVDs often store several episodes in one title. Bromelia reads the disc's menu navigation to find where each episode starts and splits the MKV with mkvmerge, without re-encoding. Episode numbers are read from the menu screens with ffmpeg and tesseract when installed; otherwise enter the first episode number when opening the disc, or episodes are numbered from 1.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            TitleRuleEditor(rule: $config.rip.titleSelection, previewInfo: previewInfo)
            Section("makemkvcon options") {
                LabeledContent("Minimum title length") {
                    OptionalIntField(title: "Seconds", value: $config.rip.minLengthSeconds, placeholder: "Setting", suffix: "seconds (--minlength)")
                }
                LabeledContent("Read cache") {
                    OptionalIntField(title: "MB", value: $config.rip.cacheMB, placeholder: "Default", suffix: "MB (--cache)")
                }
                TriStatePicker(title: "Direct disc access (--directio)", value: $config.rip.directIO)
                TextField("Extra switches", text: $config.rip.extraArguments, prompt: Text("Advanced: additional makemkvcon switches"))
                Toggle("Save disc information (disc-info.json) next to the files", isOn: $config.rip.writeDiscInfoJSON)
            }
        }
        .formStyle(.grouped)
    }

    private var modeHelp: String {
        switch config.rip.mode {
        case .mkv: return "Titles chosen by the rules below are saved as MKV files. Track selection follows the profile."
        case .backup: return "The whole disc is copied as is, still encrypted. Needs MakeMKV (or another decrypter) to play."
        case .backupDecrypted: return "The whole disc is copied and video files are decrypted, keeping menus and extras."
        case .backupThenMkv: return "A decrypted backup is made first (fast sequential read), then MKV files are made from the backup. Safest for scratched discs."
        case .infoOnly: return "Only reads the disc and stores disc-info.json. Useful for cataloguing or for scripts."
        case .audioCD: return "Audio CDs are ripped with the audio CD command."
        case .dataImage: return "Data discs are saved as ISO images."
        }
    }
}

struct TitleRuleEditor: View {
    @Binding var rule: TitleSelection
    let previewInfo: DiscInfo?

    var body: some View {
        Section("Titles to rip") {
            Picker("Choose", selection: $rule.strategy) {
                ForEach(TitleSelection.Strategy.allCases) { Text($0.label).tag($0) }
            }
            switch rule.strategy {
            case .longest:
                LabeledContent("Number of titles") {
                    Stepper(value: $rule.longestCount, in: 1...99) { Text("\(rule.longestCount)").monospacedDigit() }
                }
            case .indices:
                TextField("Pattern", text: $rule.indexPattern, prompt: Text("0,2-4,7-  ·  last  ·  all"))
                Picker("Numbers refer to", selection: $rule.indexBase) {
                    ForEach(TitleSelection.IndexBase.allCases) { Text($0.label).tag($0) }
                }
                if let err = patternError { Text(err).font(.caption).foregroundStyle(.red) }
            case .manual:
                Text("Automatic rips are not possible with this setting; open the disc and pick titles.")
                    .font(.caption).foregroundStyle(.secondary)
            case .all:
                EmptyView()
            }
        }
        Section {
            HStack {
                LabeledContent("Duration") {
                    DurationField(seconds: $rule.minDurationSeconds, placeholder: "min")
                    Text("to")
                    DurationField(seconds: $rule.maxDurationSeconds, placeholder: "max")
                }
            }
            LabeledContent("Chapters") {
                IntField(title: "Min", value: $rule.minChapters, width: 60)
                Text("to")
                IntField(title: "Max", value: $rule.maxChapters, width: 60)
            }
            LabeledContent("Size") {
                IntField(title: "Min", value: $rule.minSizeMB, width: 80)
                Text("to")
                IntField(title: "Max", value: $rule.maxSizeMB, suffix: "MB", width: 80)
            }
            TextField("Include if matching", text: $rule.includePattern, prompt: Text("Regular expression, e.g. ^(00800|00801)\\.mpls"))
            TextField("Exclude if matching", text: $rule.excludePattern, prompt: Text("Regular expression"))
            Toggle("Skip duplicate titles (same segments and length)", isOn: $rule.skipDuplicates)
            Toggle("Skip alternate angles", isOn: $rule.skipAlternateAngles)
            LabeledContent("At most") {
                IntField(title: "Max titles", value: $rule.maxTitles, suffix: "titles (0 = no limit)", width: 60)
            }
        } header: {
            Text("Filters")
        } footer: {
            Text("0 means no limit. Patterns are matched against the title name, comment, source file (e.g. 00800.mpls), output file name, segment map and “#<source id>”.")
                .font(.caption).foregroundStyle(.secondary)
        }
        if let info = previewInfo {
            Section("Preview on “\(info.name)”") {
                let result = TitleSelector.evaluate(info.titles, rule: rule)
                ForEach(result.decisions) { d in
                    let t = info.title(at: d.titleIndex)
                    HStack {
                        Image(systemName: d.selected ? "checkmark.circle.fill" : "circle")
                            .foregroundStyle(d.selected ? .green : .secondary)
                        Text("Title \(d.titleIndex)").frame(width: 60, alignment: .leading)
                        Text(t?.durationText ?? "").monospacedDigit().frame(width: 70, alignment: .leading)
                        Text("\(t?.chapterCount ?? 0) ch").frame(width: 50, alignment: .leading)
                        Text(t?.sourceFileName ?? "").foregroundStyle(.secondary)
                        Spacer()
                        Text(d.reason).font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }

    private var patternError: String? {
        do { _ = try IndexPattern(parsing: rule.indexPattern); return nil } catch { return error.localizedDescription }
    }
}

/// h:mm:ss text field bound to seconds (0 = empty).
struct DurationField: View {
    @Binding var seconds: Int
    var placeholder: String

    var body: some View {
        TextField(placeholder, text: Binding(
            get: { seconds == 0 ? "" : TitleInfo.formatDuration(seconds) },
            set: { seconds = TitleInfo.parseDuration($0.trimmingCharacters(in: .whitespaces)) }
        ), prompt: Text(placeholder))
        .labelsHidden()
        .frame(width: 80)
        .multilineTextAlignment(.trailing)
        .help("h:mm:ss, m:ss or seconds")
    }
}

// MARK: - Output

struct OutputTab: View {
    @Environment(AppModel.self) private var model
    @Binding var config: DriveConfig

    var body: some View {
        Form {
            Section("Location") {
                PathField(title: "Output folder", path: $config.output.rootOverride, kind: .directory,
                          placeholder: "Global default: \(model.config.outputRoot)")
                Picker("Layout", selection: $config.output.layout) {
                    ForEach(LibraryLayout.allCases) { Text($0.label).tag($0) }
                }
                if config.output.layout == .mediaServer {
                    Text("Movies/Name (Year)/Name (Year).mkv, TV Shows/Name (Year)/Season 02/Name (Year) - S02E05.mkv; other titles go to Other/ and backups to Backup/ (hidden from Plex and Jellyfin). Turn on the online lookup for years. The templates below aren't used.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                TextField("Folder name", text: $config.output.folderTemplate, prompt: Text(OutputConfig.defaultFolderTemplate))
                Text("Preview: \(preview(config.output.folderTemplate, file: false))")
                    .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                Picker("If the folder already exists", selection: $config.output.conflictPolicy) {
                    ForEach(ConflictPolicy.allCases) { Text($0.label).tag($0) }
                }
            }
            Section("File names") {
                TextField("Name files and backups", text: $config.output.fileNameTemplate, prompt: Text("Empty keeps MakeMKV's names"))
                if !config.output.fileNameTemplate.isEmpty {
                    Text("Movie: \(preview(config.output.fileNameTemplate, file: true, tv: false)).mkv")
                        .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                    Text("TV: \(preview(config.output.fileNameTemplate, file: true, tv: true)).mkv")
                        .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                }
                HStack {
                    Button("Use the Standard Naming") { config.output.fileNameTemplate = OutputConfig.defaultFileNameTemplate; config.output.folderTemplate = OutputConfig.defaultFolderTemplate }
                        .disabled(config.output.fileNameTemplate == OutputConfig.defaultFileNameTemplate && config.output.folderTemplate == OutputConfig.defaultFolderTemplate)
                    Spacer()
                }
                TextField("Backup subfolder (backup + MKV mode)", text: $config.output.backupSubfolder)
                Text("Standard naming: {name} - {episode} - {discLabel} - {rip} - {track} - {format}, leaving out parts that don't apply. Format codes: DVD, BR (Blu-ray), 4K (Ultra HD Blu-ray); DVDe, BRe, 4Ke for backups that are not decrypted.")
                    .font(.caption).foregroundStyle(.secondary)
                TokenReference(tokens: TemplateRenderer.fileTokens)
            }
            Section {
                Toggle("Write SHA-256 checksums (SHA256SUMS)", isOn: $config.archive.checksums)
                Toggle("Write an archive record (bromelia.json) and the job log", isOn: $config.archive.archiveRecord)
                Toggle("Check every rip against the disc listing", isOn: $config.archive.verifyRips)
            } header: {
                Text("Archiving")
            } footer: {
                Text("Checksums of every file (including backup folders) are saved in the output folder in the standard format; check a copy later with “shasum -a 256 -c SHA256SUMS”. The archive record describes the disc, titles, episodes and files with their sizes and hashes. Checking compares each MKV's length and tracks with the disc listing (needs mkvmerge) and each backup's structure. Files only reach the output folder when the job succeeded; otherwise they are kept in a folder marked [INCOMPLETE] or [READ ERRORS].")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }

    private func preview(_ template: String, file: Bool, tv: Bool = false) -> String {
        var v = TemplateRenderer.dateValues()
        let label = tv ? "SHOW_NAME_S2_D3" : "MOVIE_TITLE"
        let id = MediaIdentity(name: tv ? "Show Name" : "Movie Title", kind: tv ? .tv : .movie, format: tv ? .dvd : .bluray, encrypted: false,
                               label: LabelParser.parse(label), reason: "")
        for (k, x) in id.templateValues(rip: "Rip") { v[k] = x }
        v["disc"] = label; v["volume"] = label; v["type"] = tv ? "dvd" : "bd"; v["drive"] = config.name; v["job"] = "1a2b3c4d"
        v["title"] = id.name; v["index"] = "3"; v["n"] = "1"; v["source"] = tv ? "4" : "800"; v["duration"] = tv ? "0-23-40" : "1-58-02"
        v["chapters"] = "24"; v["original"] = "\(label)_t03"; v["comment"] = ""
        v["track"] = tv ? "Title 4" : "Playlist 00800"
        if tv { v["episode"] = "Episode 07"; v["episodeNumber"] = "7" }
        let rel = TemplateRenderer.renderPath(template, values: v)
        if file { return rel }
        let root = config.output.rootOverride.isEmpty ? model.config.outputRoot : config.output.rootOverride
        return (Paths.expandTilde(root) as NSString).appendingPathComponent(rel)
    }
}
