/// Lower-case hex, two digits a byte.
package func hex(_ bytes: some Sequence<UInt8>) -> String {
    bytes.map { b in
        let h = String(b, radix: 16)
        return h.count == 1 ? "0" + h : h
    }.joined()
}

/// A-Z lowered and nothing else, so a name folds the same on every platform.
package func asciiLower(_ s: String) -> String {
    String(String.UnicodeScalarView(s.unicodeScalars.map { $0.value >= 65 && $0.value <= 90 ? Unicode.Scalar($0.value + 32)! : $0 }))
}

/// What comes before the last '/'; "" when there is none.
package func parentFolder(of path: String) -> String {
    guard let i = path.utf8.lastIndex(of: UInt8(ascii: "/")) else { return "" }
    return String(decoding: path.utf8[..<i], as: UTF8.self)
}
