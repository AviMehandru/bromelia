import BroDomain
import BroFoundation
import BroPorts

/// SettingsIsolation for macOS and Linux (plan §10.3): each makemkvcon run gets HOME set to the job's home folder,
/// holding its own settings.conf (0600: it may hold the registration key) and the generated profile. The files stay
/// with the job, except the key: release removes the app_Key line from settings.conf, whatever wrote it, so the key
/// doesn't wait in the job's folder for retention. A run that never released its lease (the engine crashed) leaves the
/// key there; startup recovery removes it with `scrubKey(_:)`.
public final class HomeDirIsolation: SettingsIsolation {
    private let fs: any FileSystem
    private let layout: HomeLayout

    public init(fs: any FileSystem, layout: HomeLayout) {
        self.fs = fs
        self.layout = layout
    }

    public func prepare(_ settings: MakemkvRunSettings) throws(BroError) -> any IsolationLease {
        let home = settings.workDirectory
        let folder = home + (layout == .macos ? "/Library/MakeMKV" : "/.MakeMKV")
        try fs.createDirectory(folder, parentsMustExist: false)
        let conf = SettingsConf.render(settings.settings, header: "Written by Bromelia for one makemkvcon run")
        try fs.writeAtomically(folder + "/settings.conf", bytes: Array(conf.utf8), mode: 0o600)
        var profile: String?
        if let xml = settings.profileXml {
            profile = home + "/profile.mmcp.xml"
            try fs.writeAtomically(profile!, bytes: Array(xml.utf8), mode: 0o644)
        }
        return Lease(fs: fs, home: home, conf: confPath(home), profile: profile)
    }

    private func confPath(_ home: String) -> String { home + (layout == .macos ? "/Library/MakeMKV" : "/.MakeMKV") + "/settings.conf" }

    /// Removes the app_Key line from the settings.conf of a run's home (`workDirectory`), as release does: for the home
    /// of a run the engine never released (it crashed). makemkv.keyNotRemoved when the file is there but can't be
    /// rewritten; nil otherwise.
    @discardableResult
    public func scrubKey(_ workDirectory: String) -> BroMessage? { Self.scrub(fs, confPath(workDirectory)) }

    static func scrub(_ fs: any FileSystem, _ conf: String) -> BroMessage? {
        guard fs.exists(conf) else { return nil }
        do throws(BroError) {
            let text = String(decoding: try fs.read(conf), as: UTF8.self)
            let kept = text.split(separator: "\n", omittingEmptySubsequences: false).filter { !isKeyLine($0) }.joined(separator: "\n")
            if kept != text { try fs.writeAtomically(conf, bytes: Array(kept.utf8), mode: 0o600) }
            return nil
        } catch {
            let reason = error.params.first { $0.key == "reason" }?.value.string ?? error.code
            return BroMessage(.makemkvKeyNotRemoved, [("path", .string(conf)), ("reason", .string(reason))], severity: .warning)
        }
    }

    /// A settings.conf line that sets app_Key.
    static func isKeyLine(_ line: Substring) -> Bool {
        let t = line.drop { $0 == " " || $0 == "\t" }
        guard t.hasPrefix("app_Key") else { return false }
        return t.dropFirst("app_Key".count).drop { $0 == " " || $0 == "\t" }.hasPrefix("=")
    }

    private struct Lease: IsolationLease {
        let fs: any FileSystem
        let home: String
        let conf: String
        let profile: String?
        func environment() -> [String: String] { ["HOME": home] }
        func profilePath() -> String? { profile }
        func firstOutput() {}

        @discardableResult
        func release() -> BroMessage? { HomeDirIsolation.scrub(fs, conf) }
    }
}
