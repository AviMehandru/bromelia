import Foundation

/// The forum page where MakeMKV's developer posts the free beta key.
public enum BetaKeyPage {
    public static let url = "https://forum.makemkv.com/forum/viewtopic.php?f=5&t=1053"

    /// The key in the post (the T-… string in its code block, else the first one on the page); nil when there is none.
    public static func parse(_ html: String) -> String? {
        let range = NSRange(html.startIndex..., in: html)
        let inCode = try! NSRegularExpression(pattern: #"<code>\s*(T-[A-Za-z0-9@_]{20,})\s*</code>"#)
        if let m = inCode.firstMatch(in: html, range: range), let r = Range(m.range(at: 1), in: html) { return String(html[r]) }
        let anywhere = try! NSRegularExpression(pattern: #"T-[A-Za-z0-9@_]{20,}"#)
        if let m = anywhere.firstMatch(in: html, range: range), let r = Range(m.range, in: html) { return String(html[r]) }
        return nil
    }
}
