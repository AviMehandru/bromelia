import Foundation

/// MakeMKV's free beta key, replaced about once a month.
public enum BetaKey {
    /// Whether an automatic update may replace `currentKey`: only a beta key (T-…) or no key, never a purchased one.
    public static func mayReplace(_ currentKey: String?) -> Bool {
        guard let k = currentKey?.trimmingCharacters(in: .whitespacesAndNewlines), !k.isEmpty else { return true }
        return k.hasPrefix("T-")
    }
}
