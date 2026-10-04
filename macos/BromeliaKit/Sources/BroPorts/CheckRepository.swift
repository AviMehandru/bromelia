import BroDomain
import BroFoundation

/// The checks table.
public protocol CheckRepository: Sendable {
    func insert(_ check: CheckRecord) async throws(BroError)

    /// Newest first.
    func forUnit(_ unitId: Id) async throws(BroError) -> [CheckRecord]

    func latest(_ folder: String) async throws(BroError) -> CheckRecord?
}
