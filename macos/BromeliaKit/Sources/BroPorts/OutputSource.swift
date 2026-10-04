/// Which output a line came from.
public enum OutputSource: String, Sendable, CaseIterable {
    case stdout
    case stderr
}
