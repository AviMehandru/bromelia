import Foundation

/// Whether a rule's conditions hold for a disc (was PluginMatcher).
public enum RuleMatcher {
    /// Every condition given must hold; an empty `when` matches every disc. The regular expressions are
    /// case-insensitive (an invalid one never matches); a format ending in * matches every code with that prefix
    /// (BR* = BR and BRe), others match exactly, ignoring case.
    public static func matches(_ when: RuleWhen, facts: RuleFacts) -> Bool {
        if let nl = when.nameOrLabel, !nl.isEmpty, !(search(nl, facts.name) || search(nl, facts.label)) { return false }
        if let n = when.name, !n.isEmpty, !search(n, facts.name) { return false }
        if let l = when.label, !l.isEmpty, !search(l, facts.label) { return false }
        if let formats = when.formats, !formats.isEmpty,
           !formats.contains(where: { formatMatches($0.trimmingCharacters(in: .whitespaces), facts.formatCode) }) { return false }
        if let kinds = when.kinds, !kinds.isEmpty, !kinds.contains(facts.kind ?? "") { return false }
        if let drives = when.drives, !drives.isEmpty, !drives.contains(facts.driveId ?? "") { return false }
        if let profiles = when.profiles, !profiles.isEmpty, !profiles.contains(facts.profileId ?? "") { return false }
        if let automatic = when.automatic, automatic != facts.automatic { return false }
        return true
    }

    static func search(_ pattern: String, _ text: String) -> Bool {
        guard let re = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]) else { return false }
        return re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil
    }

    static func formatMatches(_ format: String, _ code: String) -> Bool {
        let c = MessageCatalog.asciiLower(code)
        if format.hasSuffix("*") { return c.hasPrefix(MessageCatalog.asciiLower(String(format.dropLast()))) }
        return MessageCatalog.asciiLower(format) == c
    }
}
