import BroDomain
import BroFoundation
import BroPorts

/// SettingsIsolation for macOS and Linux (plan §10.3): each makemkvcon run gets HOME set to the job's home folder,
/// holding its own settings.conf (0600: it may hold the registration key) and the generated profile. The files stay
/// with the job, except the key: release removes the app_Key line from settings.conf, whatever wrote it, so the key
/// doesn't wait in the job's folder for retention.
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
        return Lease(fs: fs, home: home, conf: folder + "/settings.conf", profile: profile)
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

        func release() {
            // Best effort: the file is 0600 and pruned with the job.
            guard fs.exists(conf), let bytes = try? fs.read(conf) else { return }
            let text = String(decoding: bytes, as: UTF8.self)
            let kept = text.split(separator: "\n", omittingEmptySubsequences: false).filter { !HomeDirIsolation.isKeyLine($0) }.joined(separator: "\n")
            if kept != text { try? fs.writeAtomically(conf, bytes: Array(kept.utf8), mode: 0o600) }
        }
    }
}
