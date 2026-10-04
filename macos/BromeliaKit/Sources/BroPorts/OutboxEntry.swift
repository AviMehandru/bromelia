import BroDomain
import BroFoundation

/// A row of outbox: a notification waiting to be sent, and what happened to it.
public struct OutboxEntry: Sendable, Equatable {
    public var id: Id
    public var targetId: String
    public var jobId: Id?
    public var title: BroMessage
    /// The body, one per line.
    public var lines: [BroMessage]
    public var status: StatusWord
    public var createdAt: Instant
    public var attempts: Int
    public var nextAttemptAt: Instant?
    public var sentAt: Instant?
    public var lastError: BroError?

    public init(id: Id, targetId: String, jobId: Id? = nil, title: BroMessage, lines: [BroMessage], status: StatusWord,
                createdAt: Instant, attempts: Int, nextAttemptAt: Instant? = nil, sentAt: Instant? = nil,
                lastError: BroError? = nil) {
        self.id = id
        self.targetId = targetId
        self.jobId = jobId
        self.title = title
        self.lines = lines
        self.status = status
        self.createdAt = createdAt
        self.attempts = attempts
        self.nextAttemptAt = nextAttemptAt
        self.sentAt = sentAt
        self.lastError = lastError
    }
}
