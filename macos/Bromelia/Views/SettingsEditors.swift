import SwiftUI
import UniformTypeIdentifiers
import AppKit

/// Catalog-driven editor for MakeMKV settings.conf values.
struct MakeMKVSettingsTab: View {
    enum Mode { case global, drive }
    @Environment(AppModel.self) private var model
    @Binding var settings: [String: String]
    let mode: Mode
    @State private var showAdvanced = false

    var body: some View {
        let catalog = model.catalog
        let known = Set(catalog.allSettings.map(\.key))
        Form {
            Section {
                Text(mode == .drive
                     ? "Checked settings override the global MakeMKV settings for this drive only. Each drive runs makemkvcon with its own settings.conf, so drives never share these values."
                     : "These settings apply to every drive unless a drive overrides them. They are written to a private settings.conf for each job; MakeMKV's own settings are not modified.")
                    .font(.callout).foregroundStyle(.secondary)
                Toggle("Show advanced settings", isOn: $showAdvanced)
            }
            ForEach(catalog.sections) { section in
                let items = section.settings.filter { $0.appliesToThisPlatform && (showAdvanced || !$0.isAdvanced) }
                if !items.isEmpty {
                    Section(section.title) {
                        ForEach(items) { s in
                            SettingRow(setting: s, settings: $settings, mode: mode, inherited: model.config.globalSettings[s.key])
                        }
                    }
                }
            }
            Section {
                KeyValueEditor(values: Binding(
                    get: { settings.filter { !known.contains($0.key) && $0.key != "app_Key" } },
                    set: { newValue in
                        for k in settings.keys where !known.contains(k) && k != "app_Key" { settings[k] = nil }
                        for (k, v) in newValue { settings[k] = v }
                    }
                ), keyPlaceholder: "setting_Key", valuePlaceholder: "value")
            } header: {
                Text("Additional settings")
            } footer: {
                Text("Any other settings.conf key, written as is.").font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }
}

private struct SettingRow: View {
    let setting: SettingsCatalog.Setting
    @Binding var settings: [String: String]
    let mode: MakeMKVSettingsTab.Mode
    let inherited: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(alignment: .firstTextBaseline) {
                if mode == .drive {
                    Toggle("", isOn: Binding(
                        get: { settings[setting.key] != nil },
                        set: { settings[setting.key] = $0 ? (inherited ?? setting.default ?? "") : nil }
                    ))
                    .toggleStyle(.checkbox)
                    .labelsHidden()
                    .help("Override for this drive")
                }
                control
                    .disabled(mode == .drive && settings[setting.key] == nil)
            }
            if let help = setting.help {
                Text(help).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            if mode == .drive && settings[setting.key] == nil {
                Text("Inherited: \(inheritedDescription)").font(.caption2).foregroundStyle(.tertiary)
            }
        }
    }

    private var inheritedDescription: String {
        let v = inherited ?? ""
        if v.isEmpty { return "MakeMKV default" }
        if setting.type == "bool" { return v == "1" ? "On" : "Off" }
        if let c = setting.choices?.first(where: { $0.value == v }) { return c.label }
        return v
    }

    private var value: Binding<String> {
        Binding(get: { settings[setting.key] ?? (mode == .drive ? (inherited ?? "") : "") },
                set: { settings[setting.key] = $0 })
    }

    @ViewBuilder
    private var control: some View {
        switch setting.type {
        case "bool":
            Toggle(setting.label, isOn: Binding(
                get: { (settings[setting.key] ?? inherited ?? setting.default ?? "0") == "1" },
                set: { settings[setting.key] = $0 ? "1" : "0" }
            ))
        case "choice":
            Picker(setting.label, selection: value) {
                ForEach(setting.choices ?? [], id: \.value) { c in Text(c.label).tag(c.value) }
            }
        case "file":
            PathField(title: setting.label, path: value, kind: .file)
        case "directory":
            PathField(title: setting.label, path: value, kind: .directory)
        case "selection":
            SelectionRuleField(title: setting.label, rule: value)
        case "int":
            TextField(setting.label, text: value, prompt: Text(setting.default.map { $0.isEmpty ? "Default" : $0 } ?? "Default"))
        default:
            TextField(setting.label, text: value, prompt: Text(setting.type == "language" ? "e.g. eng" : "Default"))
        }
    }
}

/// Text field for MakeMKV selection rules, with presets and a token reference.
struct SelectionRuleField: View {
    @Environment(AppModel.self) private var model
    let title: String
    @Binding var rule: String
    @State private var showHelp = false

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                TextField(title, text: $rule, prompt: Text("Empty = MakeMKV default"))
                    .font(.system(.body, design: .monospaced))
                Menu("Presets") {
                    ForEach(model.catalog.selectionPresets, id: \.name) { p in
                        Button(p.name) { rule = p.rule }
                    }
                    Divider()
                    Button("Clear") { rule = "" }
                }
                .fixedSize()
                Button { showHelp.toggle() } label: { Image(systemName: "questionmark.circle") }
                    .buttonStyle(.borderless)
                    .popover(isPresented: $showHelp) { SelectionHelp().padding().frame(width: 460) }
            }
            if let issue = SelectionRuleLint.check(rule) {
                Text(issue).font(.caption).foregroundStyle(.orange)
            }
        }
    }
}

enum SelectionRuleLint {
    static func check(_ rule: String) -> String? {
        let r = rule.trimmingCharacters(in: .whitespaces)
        if r.isEmpty { return nil }
        var depth = 0
        for c in r {
            if c == "(" { depth += 1 }
            if c == ")" { depth -= 1; if depth < 0 { return "Unbalanced parentheses" } }
        }
        if depth != 0 { return "Unbalanced parentheses" }
        for item in r.split(separator: ",") {
            let s = item.trimmingCharacters(in: .whitespaces)
            guard let colon = s.firstIndex(of: ":") else { return "“\(s)” is missing ':' (expected e.g. +sel:all)" }
            let action = s[..<colon]
            let valid = action == "+sel" || action == "-sel" || action.first.map { "+-=".contains($0) } == true
            if !valid { return "“\(action)” is not a valid action (+sel, -sel, +N, -N, =N)" }
        }
        return nil
    }
}

private struct SelectionHelp: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Selection rules").font(.headline)
            Text("A comma separated list of actions applied in order to every track. Each item is ‘action:condition’. Actions: +sel (select), -sel (deselect), +N / -N (add to / subtract from the track's weight), =N (set weight). Conditions combine tokens with | (or), & (and), ! (not) and parentheses.")
                .font(.callout).fixedSize(horizontal: false, vertical: true)
            Grid(alignment: .leading, horizontalSpacing: 10, verticalSpacing: 2) {
                ForEach(model.catalog.selectionTokens, id: \.token) { t in
                    GridRow {
                        Text(t.token).font(.system(.caption, design: .monospaced))
                        Text(t.help).font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            Text("Example: -sel:all,+sel:(favlang|nolang|single),-sel:(havemulti|havecore),-sel:mvcvideo,=100:all,-10:favlang")
                .font(.system(.caption, design: .monospaced)).textSelection(.enabled)
        }
    }
}

// MARK: - Profile

struct ProfileTab: View {
    @Binding var profile: ProfileConfig
    @State private var showXML = false

    var body: some View {
        Form {
            Section {
                Picker("Profile", selection: $profile.mode) {
                    ForEach(ProfileMode.allCases) { Text($0.label).tag($0) }
                }
                Text("A MakeMKV profile controls which tracks are selected by default, MKV flags and audio conversion. It is passed to makemkvcon with --profile.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            switch profile.mode {
            case .makemkvDefault:
                EmptyView()
            case .customFile:
                Section("Custom profile") {
                    PathField(title: "Profile file", path: $profile.customPath, kind: .file, placeholder: "/path/to/profile.mmcp.xml")
                    Button("Start from the Bromelia template…") { exportTemplate() }
                }
            case .generated:
                Section("Track selection") {
                    TextField("Profile name", text: $profile.generated.name)
                    SelectionRuleField(title: "Selection rule", rule: $profile.generated.selectionRule)
                }
                Section("MKV flags") {
                    Toggle("Mark the first audio track as default", isOn: $profile.generated.setFirstAudioTrackAsDefault)
                    Toggle("Mark the first subtitle track as default", isOn: $profile.generated.setFirstSubtitleTrackAsDefault)
                    Toggle("Mark the first forced subtitle track as default", isOn: $profile.generated.setFirstForcedSubtitleTrackAsDefault)
                    Toggle("Ignore the disc's forced subtitle flag", isOn: $profile.generated.ignoreForcedSubtitlesFlag)
                    Toggle("Use ISO 639-2/T language codes (deu instead of ger)", isOn: $profile.generated.useISO639Type2T)
                    Toggle("Insert chapter 0 when the first chapter does not start at 0", isOn: $profile.generated.insertFirstChapter00IfMissing)
                }
                Section("Audio conversion") {
                    Picker("Stereo / mono LPCM", selection: $profile.generated.lpcmStereo) {
                        ForEach(LPCMOutput.allCases) { Text($0.label).tag($0) }
                    }
                    Picker("Multichannel LPCM", selection: $profile.generated.lpcmMultichannel) {
                        ForEach(LPCMOutput.allCases) { Text($0.label).tag($0) }
                    }
                }
                Section {
                    DisclosureGroup("Generated profile XML", isExpanded: $showXML) {
                        ScrollView {
                            Text(ProfileBuilder.build(profile.generated))
                                .font(.system(.caption, design: .monospaced))
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .frame(height: 240)
                    }
                    Button("Export Profile…") { exportGenerated() }
                }
            }
        }
        .formStyle(.grouped)
    }

    private func save(_ text: String, name: String) -> URL? {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = name
        panel.allowedContentTypes = [.xml]
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        try? text.write(to: url, atomically: true, encoding: .utf8)
        return url
    }

    private func exportGenerated() {
        _ = save(ProfileBuilder.build(profile.generated), name: "\(profile.generated.name).mmcp.xml")
    }

    private func exportTemplate() {
        if let url = save(ProfileBuilder.build(GeneratedProfile()), name: "custom.mmcp.xml") {
            profile.customPath = url.path
        }
    }
}
