import BroFoundation

/// The effective profile of a disc (plan §24).
public enum ProfileResolver {
    /// Layers, lowest first, each merged like an RFC 7386 merge patch (null removes a key, so its default comes
    /// back): every field at its default → the default profile → the drive's profile → the drive's own MakeMKV
    /// settings → each enabled rule that matches, in order (its then.set; then.profile replaces the drive's profile
    /// and later rules still apply) → the session's choices. Steps are the profile's, then the rules', each once.
    /// `driveId` is the drive entry's id (nil: no entry, or an ISO / folder).
    public static func resolve(_ config: Config, driveId: String?, facts: RuleFacts, sessionChoices: JsonValue?) -> EffectiveProfile {
        let doc = config.document
        let profiles = Config.profiles(config)
        func profileJson(_ id: String) -> JsonValue? { profiles.first { $0.id == id }?.json }
        let defaultId = doc["defaultProfile"]?.string ?? "default"
        let drive = driveId.flatMap { id in Config.drives(config).first { $0.id == id } }
        var driveProfileId = drive?.profile.flatMap { $0 == defaultId ? nil : $0 }

        var ruleLayers: [Rule] = []
        var ruleSteps: [String] = []
        var currentProfile = driveProfileId ?? defaultId
        for rule in Config.rules(config) where rule.enabled {
            var f = facts
            f.driveId = driveId
            f.profileId = currentProfile
            guard RuleMatcher.matches(rule.when, facts: f) else { continue }
            if let switchTo = rule.then.profile {
                driveProfileId = switchTo == defaultId ? nil : switchTo
                currentProfile = switchTo
            }
            ruleLayers.append(rule)
            ruleSteps += rule.then.steps
        }

        var trace: [ResolveTrace] = []
        var issues: [Issue] = []
        let node = SchemaWalker.def("ProfileFields")
        var profile = SchemaWalker.normalize(.object([]), node, "", fill: true, full: true, &issues)
        trace.append(ResolveTrace(layer: .defaults, id: nil, keys: []))
        func layer(_ l: ResolveLayer, _ id: String?, _ patch: JsonValue?) {
            let p = strip(patch)
            profile = mergePatch(profile, p)
            trace.append(ResolveTrace(layer: l, id: id, keys: leafKeys(p, "")))
        }
        layer(.defaultProfile, defaultId, profileJson(defaultId))
        if let id = driveProfileId { layer(.driveProfile, id, profileJson(id)) }
        if let drive {
            // In the document's order (DriveEntry's dictionary has none).
            let settings = doc["drives"]?.array?.first { $0["id"]?.string == drive.id }?["makemkvSettings"]
            if let s = settings, !(s.members ?? []).isEmpty {
                layer(.driveOverrides, drive.id, .object([("makemkv", .object([("settings", s)]))]))
            } else {
                layer(.driveOverrides, drive.id, .object([]))
            }
        }
        for rule in ruleLayers { layer(.rule, rule.id, rule.then.set) }
        if let sessionChoices { layer(.session, nil, sessionChoices) }
        // A key a patch removed takes its default again.
        profile = SchemaWalker.normalize(profile, node, "", fill: true, full: true, &issues)

        var steps: [String] = []
        for s in (profile["steps"]?.array ?? []).map({ $0.string ?? "" }) + ruleSteps where !steps.contains(s) { steps.append(s) }
        return EffectiveProfile(profile: profile, steps: steps, trace: trace)
    }

    /// A profile as a patch: without its id and name.
    static func strip(_ p: JsonValue?) -> JsonValue {
        guard let members = p?.members else { return .object([]) }
        return .object(members.filter { $0.key != "id" && $0.key != "name" })
    }

    /// RFC 7386: objects merge key by key, null removes a key, anything else replaces.
    static func mergePatch(_ target: JsonValue, _ patch: JsonValue) -> JsonValue {
        guard let p = patch.members else { return patch }
        var members = target.members ?? []
        for m in p {
            let at = members.firstIndex { $0.key == m.key }
            if case .null = m.value {
                if let at { members.remove(at: at) }
                continue
            }
            let merged = mergePatch(at.map { members[$0].value } ?? .object([]), m.value)
            if let at { members[at] = (m.key, merged) } else { members.append((m.key, merged)) }
        }
        return .object(members)
    }

    static func leafKeys(_ v: JsonValue, _ prefix: String) -> [String] {
        (v.members ?? []).flatMap { m -> [String] in
            let path = prefix.isEmpty ? m.key : prefix + "." + m.key
            if let inner = m.value.members, !inner.isEmpty { return leafKeys(m.value, path) }
            return [path]
        }
    }
}

