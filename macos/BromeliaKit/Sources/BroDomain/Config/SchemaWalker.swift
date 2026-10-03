import BroFoundation
import Foundation

/// Walks the compiled configuration schema (`ConfigSchema`): fills defaults, orders keys, drops unknown keys and
/// checks values. The same algorithm as tools/check-contracts.py's expand_defaults, so every platform decodes a
/// document the same way.
enum SchemaWalker {
    static let document: JsonValue = JsonValue.parse(ConfigSchema.text)!

    static func def(_ name: String) -> JsonValue { document["$defs"]![name]! }

    /// Follows $ref.
    static func resolve(_ node: JsonValue) -> JsonValue {
        var n = node
        for _ in 0..<20 {
            guard let r = n["$ref"]?.string else { break }
            n = def(String(r.dropFirst("#/$defs/".count)))
        }
        return n
    }

    static func branches(_ node: JsonValue, _ keys: [String]) -> [JsonValue] { keys.flatMap { node[$0]?.array ?? [] } }

    /// The object-typed branch of a node (through $ref, allOf, oneOf, anyOf), or nil.
    static func objectVariant(_ node: JsonValue) -> JsonValue? {
        let n = resolve(node)
        if n["properties"] != nil { return n }
        for sub in branches(n, ["allOf", "oneOf", "anyOf"]) { if let found = objectVariant(sub) { return found } }
        return nil
    }

    static func arrayItems(_ node: JsonValue) -> JsonValue? {
        let n = resolve(node)
        if let items = n["items"] { return items }
        for sub in branches(n, ["oneOf", "anyOf", "allOf"]) { if let found = arrayItems(sub) { return found } }
        return nil
    }

    /// The additionalProperties schema of a map (MakemkvSettings, environment), or nil.
    static func mapValues(_ node: JsonValue) -> JsonValue? {
        let n = resolve(node)
        if let ap = n["additionalProperties"], case .object = ap { return ap }
        for sub in branches(n, ["allOf", "oneOf", "anyOf"]) { if let found = mapValues(sub) { return found } }
        return nil
    }

    static func inherits(_ node: JsonValue) -> Bool {
        let n = resolve(node)
        if n["x-inherit"]?.bool == true { return true }
        return (n["allOf"]?.array ?? []).contains(where: inherits)
    }

    static func isObject(_ node: JsonValue) -> Bool {
        let n = resolve(node)
        return n["type"]?.string == "object" || n["properties"] != nil
    }

    static func nullable(_ node: JsonValue) -> Bool {
        let n = resolve(node)
        if n["type"]?.string == "null" || (n["type"]?.array ?? []).contains(.string("null")) { return true }
        return branches(n, ["oneOf", "anyOf"]).contains(where: nullable)
    }

    static func defaultOf(_ prop: JsonValue) -> JsonValue? { prop["default"] ?? resolve(prop)["default"] }

    /// Whether the schema forbids `key` in this object (allOf: if … then not required), so it mustn't be filled: a
    /// command step gets no handbrake object.
    static func forbidden(_ obj: JsonValue, _ key: String, _ value: JsonValue) -> Bool {
        for sub in obj["allOf"]?.array ?? [] {
            guard let cond = sub["if"], let required = sub["then"]?["not"]?["required"]?.array, required.contains(.string(key)) else { continue }
            var l: [Issue] = []
            validate(value, cond, "", &l)
            if l.isEmpty { return true }
        }
        return false
    }

    static func join(_ path: String, _ key: String) -> String { path.isEmpty ? key : path + "." + key }

    /// The value with the schema's keys in order, unknown keys dropped (each reported, in document order), and,
    /// unless `fill` is false or the node is sparse (x-inherit), missing keys filled with their defaults (an object
    /// without a default becomes {} and is filled in turn). With `full`, sparse objects are filled too: the bottom
    /// layer of ProfileResolver.resolve, every profile field at its default.
    static func normalize(_ value: JsonValue, _ node: JsonValue, _ path: String, fill: Bool, full: Bool = false, _ issues: inout [Issue]) -> JsonValue {
        switch value {
        case .object(let members):
            if let obj = objectVariant(node) {
                let f = fill && (full || (!inherits(node) && !inherits(obj)))
                let props = obj["properties"]?.members ?? []
                var out: [(key: String, value: JsonValue)] = []
                var childIssues: [String: [Issue]] = [:]
                for (key, prop) in props {
                    var child = value[key]
                    if child == nil {
                        guard f, !forbidden(obj, key, value) else { continue }
                        if let d = defaultOf(prop) {
                            child = d
                        } else if key == "background" && obj["x-default-background"]?.bool == true {
                            // A step's background defaults by its kind: true for handbrake, false for command.
                            child = .bool(value["kind"]?.string == "handbrake")
                        } else if isObject(prop) && !nullable(prop) {
                            child = .object([])
                        } else {
                            continue
                        }
                    }
                    var list: [Issue] = []
                    out.append((key, normalize(child!, prop, join(path, key), fill: f, full: full, &list)))
                    childIssues[key] = list
                }
                // Issues in document order: an unknown key where it was written, a known key's own issues there too.
                for m in members {
                    if let list = childIssues[m.key] {
                        issues += list
                    } else if !props.contains(where: { $0.key == m.key }) {
                        issues.append(Issue(path: join(path, m.key), code: .configUnknownKey, params: [], severity: .warning))
                    }
                }
                return .object(out)
            }
            if let values = mapValues(node) {
                return .object(members.map { ($0.key, normalize($0.value, values, join(path, $0.key), fill: fill, full: full, &issues)) })
            }
            return value
        case .array(let items):
            guard let itemNode = arrayItems(node) else { return value }
            return .array(items.enumerated().map { normalize($0.element, itemNode, "\(path)[\($0.offset)]", fill: fill, full: full, &issues) })
        default:
            return value
        }
    }

    // MARK: Validation

    /// The issues of a value against a node: type, const, enum, required, minimum / maximum, pattern, minProperties,
    /// the branches of oneOf / anyOf, allOf, if / then and not. A SecretRef position holding text gives
    /// config.secretInline.
    static func validate(_ value: JsonValue, _ node: JsonValue, _ path: String, _ issues: inout [Issue]) {
        let n = resolve(node)
        let alternatives = branches(n, ["oneOf", "anyOf"])
        if !alternatives.isEmpty {
            let results = alternatives.map { b -> (JsonValue, [Issue]) in
                var l: [Issue] = []
                validate(value, b, path, &l)
                return (b, l)
            }
            if !results.contains(where: { $0.1.isEmpty }) {
                if case .string = value, alternatives.contains(where: { $0["$ref"]?.string == "#/$defs/SecretRef" }) {
                    issues.append(error(path, .configSecretInline))
                } else if let typed = results.first(where: { typeMatches(value, resolve($0.0)) }) {
                    issues += typed.1
                } else {
                    issues.append(invalid(path, value))
                }
            }
        }
        for sub in n["allOf"]?.array ?? [] {
            if let cond = sub["if"] {
                var l: [Issue] = []
                validate(value, cond, path, &l)
                if l.isEmpty, let then = sub["then"] { validate(value, then, path, &issues) }
                continue
            }
            validate(value, sub, path, &issues)
        }
        if let not = n["not"] {
            var l: [Issue] = []
            validate(value, not, path, &l)
            if l.isEmpty { issues.append(invalid(path, value)) }
        }
        if !typeMatches(value, n) { issues.append(invalid(path, value)); return }
        if let c = n["const"], c != value { issues.append(invalid(path, value)); return }
        if let e = n["enum"]?.array, !e.contains(value) { issues.append(invalid(path, value)); return }
        switch value {
        case .object(let members):
            for r in n["required"]?.array ?? [] {
                if let key = r.string, value[key] == nil { issues.append(error(join(path, key), .configRequired)) }
            }
            if let min = n["minProperties"]?.int, members.count < min { issues.append(invalid(path, value)) }
            for m in members {
                if let prop = n["properties"]?[m.key] {
                    validate(m.value, prop, join(path, m.key), &issues)
                } else if let ap = n["additionalProperties"], case .object = ap {
                    validate(m.value, ap, join(path, m.key), &issues)
                }
            }
        case .array(let items):
            if let itemNode = n["items"] {
                for (i, item) in items.enumerated() { validate(item, itemNode, "\(path)[\(i)]", &issues) }
            }
        case .integer, .number:
            let x = value.double ?? 0
            if let lo = n["minimum"]?.double, x < lo { issues.append(invalid(path, value)) } else if let hi = n["maximum"]?.double, x > hi {
                issues.append(invalid(path, value))
            }
        case .string(let s):
            if let pattern = n["pattern"]?.string, let re = try? NSRegularExpression(pattern: pattern),
               re.firstMatch(in: s, range: NSRange(s.startIndex..., in: s)) == nil {
                issues.append(invalid(path, value))
            }
        default:
            break
        }
    }

    static func typeMatches(_ value: JsonValue, _ node: JsonValue) -> Bool {
        guard let t = node["type"] else { return true }
        let types = t.array?.compactMap(\.string) ?? [t.string ?? ""]
        return types.contains { type in
            switch (type, value) {
            case ("object", .object), ("array", .array), ("string", .string), ("boolean", .bool), ("integer", .integer),
                 ("number", .integer), ("number", .number), ("null", .null): return true
            default: return false
            }
        }
    }

    static func error(_ path: String, _ code: MessageCode, _ params: [(key: String, value: JsonValue)] = []) -> Issue {
        Issue(path: path, code: code, params: params, severity: .error)
    }

    static func invalid(_ path: String, _ value: JsonValue) -> Issue {
        let text: String
        if case .string(let s) = value { text = s } else { text = value.description.trimmingCharacters(in: .newlines) }
        return error(path, .configInvalidValue, [("value", .string(text))])
    }
}
