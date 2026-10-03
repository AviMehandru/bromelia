/// How titles are chosen after the filters.
public enum TitleStrategy: String, Sendable, CaseIterable {
    case all
    case longest
    case indices
    case manual
}
