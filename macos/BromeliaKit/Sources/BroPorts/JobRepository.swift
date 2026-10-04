import BroDomain
import BroFoundation

/// The jobs table.
public protocol JobRepository: Sendable {
    func insert(_ job: JobRecord) async throws(BroError)

    func update(_ job: JobRecord) async throws(BroError)

    func load(_ jobId: Id) async throws(BroError) -> JobRecord?

    /// Jobs that haven't finished, by queue and position.
    func active() async throws(BroError) -> [JobRecord]

    func query(_ query: JobQuery) async throws(BroError) -> [JobRecord]
}
