import SwiftUI
import AppKit
import UniformTypeIdentifiers

/// A text field with a “Choose…” button that opens an NSOpenPanel.
struct PathField: View {
    enum Kind { case file, directory, fileOrDirectory }
    let title: String
    @Binding var path: String
    var kind: Kind = .file
    var placeholder: String = ""
    var allowedTypes: [UTType] = []

    var body: some View {
        HStack {
            TextField(title, text: $path, prompt: Text(placeholder))
            Button("Choose…") { choose() }
            if !path.isEmpty {
                Button {
                    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: Paths.expandTilde(path))])
                } label: { Image(systemName: "magnifyingglass") }
                .buttonStyle(.borderless)
                .help("Reveal in Finder")
            }
        }
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = kind != .directory
        panel.canChooseDirectories = kind != .file
        panel.canCreateDirectories = kind != .file
        panel.allowsMultipleSelection = false
        panel.showsHiddenFiles = false
        panel.treatsFilePackagesAsDirectories = true
        if !allowedTypes.isEmpty { panel.allowedContentTypes = allowedTypes }
        let current = Paths.expandTilde(path)
        if !current.isEmpty { panel.directoryURL = URL(fileURLWithPath: current).deletingLastPathComponent() }
        if panel.runModal() == .OK, let url = panel.url { path = url.path }
    }
}

/// Integer text field bound to an Int.
struct IntField: View {
    let title: String
    @Binding var value: Int
    var suffix: String = ""
    var width: CGFloat = 80

    var body: some View {
        HStack(spacing: 4) {
            TextField(title, value: $value, format: .number)
                .labelsHidden()
                .frame(width: width)
                .multilineTextAlignment(.trailing)
            if !suffix.isEmpty { Text(suffix).foregroundStyle(.secondary) }
        }
    }
}

/// Optional integer (empty = nil).
struct OptionalIntField: View {
    let title: String
    @Binding var value: Int?
    var placeholder = "Default"
    var suffix = ""

    var body: some View {
        HStack(spacing: 4) {
            TextField(title, text: Binding(
                get: { value.map(String.init) ?? "" },
                set: { value = Int($0.trimmingCharacters(in: .whitespaces)) }
            ), prompt: Text(placeholder))
            .labelsHidden()
            .frame(width: 90)
            .multilineTextAlignment(.trailing)
            if !suffix.isEmpty { Text(suffix).foregroundStyle(.secondary) }
        }
    }
}

/// Picker for an optional boolean: Default / On / Off.
struct TriStatePicker: View {
    let title: String
    @Binding var value: Bool?

    var body: some View {
        Picker(title, selection: Binding(
            get: { value.map { $0 ? 1 : 0 } ?? -1 },
            set: { value = $0 == -1 ? nil : ($0 == 1) }
        )) {
            Text("MakeMKV default").tag(-1)
            Text("On").tag(1)
            Text("Off").tag(0)
        }
    }
}

/// Collapsible reference of template tokens.
struct TokenReference: View {
    let tokens: [(String, String)]
    @State private var expanded = false

    var body: some View {
        DisclosureGroup("Available tokens", isExpanded: $expanded) {
            Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 3) {
                ForEach(tokens, id: \.0) { t in
                    GridRow {
                        Text("{\(t.0)}").font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                        Text(t.1).font(.caption).foregroundStyle(.secondary)
                    }
                }
                GridRow {
                    Text("{n:3}").font(.system(.caption, design: .monospaced))
                    Text("Zero-pad a number to 3 digits").font(.caption).foregroundStyle(.secondary)
                }
                GridRow {
                    Text("{token?text}").font(.system(.caption, design: .monospaced))
                    Text("Insert text only when token is not empty").font(.caption).foregroundStyle(.secondary)
                }
            }
            .padding(.top, 4)
        }
    }
}

struct StatusBadge: View {
    let text: String
    let color: Color

    var body: some View {
        Text(text)
            .font(.caption.weight(.medium))
            .padding(.horizontal, 7)
            .padding(.vertical, 2)
            .background(color.opacity(0.15), in: Capsule())
            .foregroundStyle(color)
    }
}

extension JobState {
    var color: Color {
        switch self {
        case .queued: return .secondary
        case .waiting: return .orange
        case .running: return .accentColor
        case .succeeded: return .green
        case .completedWithErrors: return .orange
        case .failed: return .red
        case .cancelled: return .gray
        }
    }

    var symbol: String {
        switch self {
        case .queued: return "clock"
        case .waiting: return "timer"
        case .running: return "arrow.triangle.2.circlepath"
        case .succeeded: return "checkmark.circle.fill"
        case .completedWithErrors: return "exclamationmark.triangle.fill"
        case .failed: return "xmark.octagon.fill"
        case .cancelled: return "stop.circle"
        }
    }
}

extension RobotMessage.Severity {
    var color: Color {
        switch self {
        case .debug: return .secondary
        case .info: return .primary
        case .warning: return .orange
        case .error: return .red
        }
    }
}

/// Key/value editor for string dictionaries (environment variables, extra MakeMKV settings).
struct KeyValueEditor: View {
    @Binding var values: [String: String]
    var keyPlaceholder = "NAME"
    var valuePlaceholder = "value"
    @State private var newKey = ""
    @State private var newValue = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(values.keys.sorted(), id: \.self) { key in
                HStack {
                    Text(key).font(.system(.body, design: .monospaced)).frame(minWidth: 140, alignment: .leading)
                    TextField("", text: Binding(get: { values[key] ?? "" }, set: { values[key] = $0 }))
                    Button { values[key] = nil } label: { Image(systemName: "minus.circle") }
                        .buttonStyle(.borderless)
                }
            }
            HStack {
                TextField(keyPlaceholder, text: $newKey).frame(minWidth: 140)
                TextField(valuePlaceholder, text: $newValue)
                Button {
                    let k = newKey.trimmingCharacters(in: .whitespaces)
                    guard !k.isEmpty else { return }
                    values[k] = newValue
                    newKey = ""
                    newValue = ""
                } label: { Image(systemName: "plus.circle") }
                .buttonStyle(.borderless)
                .disabled(newKey.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
    }
}

func revealInFinder(_ path: String) {
    let url = URL(fileURLWithPath: path)
    if FileManager.default.fileExists(atPath: url.path) {
        NSWorkspace.shared.activateFileViewerSelecting([url])
    } else {
        NSWorkspace.shared.open(url.deletingLastPathComponent())
    }
}
