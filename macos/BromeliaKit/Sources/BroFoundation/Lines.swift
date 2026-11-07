/// `text` cut at each LF and nothing else: a CRLF line keeps its CR. By scalars, as "\r\n" is one `Character` in Swift, so
/// `split(separator: "\n")` never finds the LF of a CRLF line.
package func lines(of text: String) -> [String] {
    text.unicodeScalars.split(separator: "\n", omittingEmptySubsequences: false).map { String($0) }
}
