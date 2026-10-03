/// One layer of an effective profile: which layer, its profile, drive or rule id, and the profile keys it set
/// (dotted paths; the API's ResolveStep).
public struct ResolveTrace: Sendable, Equatable {
    public var layer: ResolveLayer
    public var id: String?
    public var keys: [String]

    public init(layer: ResolveLayer, id: String?, keys: [String]) {
        self.layer = layer
        self.id = id
        self.keys = keys
    }
}
