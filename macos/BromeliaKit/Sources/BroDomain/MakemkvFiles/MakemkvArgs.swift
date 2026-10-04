import BroFoundation

/// makemkvcon command lines.
public enum MakemkvArgs {
    /// A drive is dev:device when the device is known (stable when drives are renumbered), else disc:N.
    static func source(_ s: MakemkvSource) -> String {
        switch s {
        case .drive(let index, let device): return device.isEmpty ? "disc:\(index)" : "dev:" + device
        case .iso(let path): return "iso:" + path
        case .file(let path): return "file:" + path
        }
    }

    static func common(_ options: MakemkvOptions) -> [String] {
        var a = ["-r", "--progress=-same", "--messages=-stdout"]
        if !options.scan { a.append("--noscan") }
        if let p = options.profilePath { a.append("--profile=" + p) }
        if let m = options.minLengthSeconds { a.append("--minlength=\(m)") }
        if let c = options.cacheMB, c > 0 { a.append("--cache=\(c)") }
        if let d = options.directIO { a.append("--directio=" + (d ? "true" : "false")) }
        return a + ArgumentSplitter.split(options.extraArguments)
    }

    /// `… info source`.
    public static func info(_ source: MakemkvSource, options: MakemkvOptions) -> [String] { common(options) + ["info", self.source(source)] }

    /// `… mkv source title destination` (title: an index or "all").
    public static func mkv(_ source: MakemkvSource, title: String, destination: String, options: MakemkvOptions) -> [String] {
        common(options) + ["mkv", self.source(source), title, destination]
    }

    /// `… backup [--decrypt] disc:N destination`. Fails with backup.needsDrive for an image or folder: backup takes
    /// disc:N only.
    public static func backup(_ source: MakemkvSource, decrypt: Bool, destination: String, options: MakemkvOptions) throws(BroError) -> [String] {
        guard case .drive(let index, _) = source else { throw BroMessage(.backupNeedsDrive, severity: .error).toError() }
        return common(options) + ["backup"] + (decrypt ? ["--decrypt"] : []) + ["disc:\(index)", destination]
    }

    /// The drive scan: `-r --cache=1 info disc:9999`.
    public static func scanDrives() -> [String] { ["-r", "--cache=1", "info", "disc:9999"] }
}
