import BroDomain
import BroFoundation
import BroPorts

/// SettingsIsolation for macOS and Linux (plan §10.3): each makemkvcon run gets HOME set to the job's home folder,
/// holding its own settings.conf (0600: it may hold the registration key) and the generated profile. The files stay
/// with the job.
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
        return Lease(home: home, profile: profile)
    }

    private struct Lease: IsolationLease {
        let home: String
        let profile: String?
        func environment() -> [String: String] { ["HOME": home] }
        func profilePath() -> String? { profile }
        func firstOutput() {}
        func release() {}
    }
}
