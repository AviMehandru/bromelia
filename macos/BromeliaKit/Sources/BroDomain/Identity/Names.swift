/// Comparing names of shows and movies.
enum Names {
    /// Lower case, letters and digits only: "Kauboi-Bibappu!" → "kauboibibappu". Per Unicode scalar, with simple
    /// lower-case mappings, as on the other platforms.
    static func normalize(_ s: String) -> String {
        var out = String.UnicodeScalarView()
        for u in s.unicodeScalars {
            switch u.properties.generalCategory {
            case .uppercaseLetter, .lowercaseLetter, .titlecaseLetter, .modifierLetter, .otherLetter, .decimalNumber:
                let lower = u.properties.lowercaseMapping.unicodeScalars
                out.append(contentsOf: lower.count == 1 ? lower : String.UnicodeScalarView([u]))
            default:
                break
            }
        }
        return String(out)
    }
}
