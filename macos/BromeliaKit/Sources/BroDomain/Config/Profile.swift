import BroFoundation

/// A profile: sparse, as written in the configuration (a missing field is inherited; see ProfileResolver), so it
/// stays a JSON object. Its id is empty for a template not added yet.
public struct Profile: Sendable, Equatable {
    public var json: JsonValue

    public init(_ json: JsonValue) { self.json = json }

    public var id: String { json["id"]?.string ?? "" }
    public var name: String { json["name"]?.string ?? "" }
}
