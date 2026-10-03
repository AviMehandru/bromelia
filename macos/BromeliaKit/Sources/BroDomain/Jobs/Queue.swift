/// The scheduler's three queues (plan §21).
public enum Queue: String, Sendable, CaseIterable, Codable {
    case acquisition
    case processing
    case maintenance
}
