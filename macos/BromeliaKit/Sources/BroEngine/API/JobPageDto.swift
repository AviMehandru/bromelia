import BroFoundation

/// A page of jobs (dtos.json JobPage): newest first; `next` is the cursor of the next page, nil on the last; `total`
/// counts every job the query matches.
public struct JobPageDto: Sendable, Equatable {
    public var items: [JobDto]
    public var next: String?
    public var total: Int

    public init(items: [JobDto], next: String?, total: Int) {
        self.items = items
        self.next = next
        self.total = total
    }

    public static func json(_ page: JobPageDto) -> JsonValue {
        .object([("items", .array(page.items.map(JobDto.json))), ("next", page.next.map { .string($0) } ?? .null),
                 ("total", .integer(Int64(page.total)))])
    }
}
