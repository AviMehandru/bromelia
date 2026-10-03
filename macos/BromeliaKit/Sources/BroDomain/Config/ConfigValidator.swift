import BroFoundation
import Foundation

/// Every problem of a configuration, with its JSON path: the issues decoding found (unknown keys), values the schema
/// doesn't allow (config.invalidValue, config.required; config.secretInline for a secret written into the
/// document), regular expressions that don't compile, references to entries that don't exist, ids used twice, and
/// the server's rules (a network address needs a token; TLS needs both files).
public enum ConfigValidator {
    public static func validate(_ config: Config) -> [Issue] {
        var issues = config.issues
        let doc = config.document
        SchemaWalker.validate(doc, SchemaWalker.document, "", &issues)
        func at(_ list: String, _ i: Int, _ rest: String) -> String { "\(list)[\(i)]" + (rest.isEmpty ? "" : "." + rest) }
        func regex(_ v: JsonValue?, _ path: String) {
            guard let pattern = v?.string, !pattern.isEmpty else { return }
            if (try? NSRegularExpression(pattern: pattern, options: .caseInsensitive)) == nil {
                issues.append(SchemaWalker.error(path, .configInvalidRegex))
            }
        }
        func titles(_ profile: JsonValue?, _ path: String) {
            regex(profile?["titles"]?["includePattern"], path + ".titles.includePattern")
            regex(profile?["titles"]?["excludePattern"], path + ".titles.excludePattern")
        }
        var ids: [String: Set<String>] = [:]
        for kind in ["libraries", "profiles", "drives", "steps", "targets", "rules"] {
            var seen = Set<String>()
            for (i, e) in (doc[kind]?.array ?? []).enumerated() {
                if let id = e["id"]?.string, !seen.insert(id).inserted {
                    issues.append(SchemaWalker.error(at(kind, i, "id"), .configDuplicateId, [("id", .string(id))]))
                }
            }
            ids[kind] = seen
        }
        func need(_ kind: String, _ noun: String, _ v: JsonValue?, _ path: String) {
            if let id = v?.string, !(ids[kind]?.contains(id) ?? false) {
                issues.append(SchemaWalker.error(path, .configUnknownReference, [("kind", .string(noun)), ("id", .string(id))]))
            }
        }
        func needAll(_ kind: String, _ noun: String, _ list: JsonValue?, _ path: String) {
            for (i, v) in (list?.array ?? []).enumerated() { need(kind, noun, v, "\(path)[\(i)]") }
        }
        need("libraries", "library", doc["defaultLibrary"], "defaultLibrary")
        need("profiles", "profile", doc["defaultProfile"], "defaultProfile")
        for (i, p) in (doc["profiles"]?.array ?? []).enumerated() {
            need("libraries", "library", p["library"], at("profiles", i, "library"))
            needAll("steps", "step", p["steps"], at("profiles", i, "steps"))
            needAll("targets", "target", p["archive"]?["replicateTo"], at("profiles", i, "archive.replicateTo"))
            titles(p, at("profiles", i, ""))
        }
        for (i, d) in (doc["drives"]?.array ?? []).enumerated() { need("profiles", "profile", d["profile"], at("drives", i, "profile")) }
        for (i, r) in (doc["rules"]?.array ?? []).enumerated() {
            for key in ["nameOrLabel", "name", "label"] { regex(r["when"]?[key], at("rules", i, "when." + key)) }
            needAll("profiles", "profile", r["when"]?["profiles"], at("rules", i, "when.profiles"))
            needAll("drives", "drive", r["when"]?["drives"], at("rules", i, "when.drives"))
            need("profiles", "profile", r["then"]?["profile"], at("rules", i, "then.profile"))
            needAll("steps", "step", r["then"]?["steps"], at("rules", i, "then.steps"))
            titles(r["then"]?["set"], at("rules", i, "then.set"))
        }
        let server = doc["server"]
        if server?["tcp"]?["enabled"]?.bool == true, let address = server?["tcp"]?["address"]?.string, address != "127.0.0.1",
           (server?["tokens"]?.array ?? []).isEmpty {
            issues.append(SchemaWalker.error("server.tcp.address", .configNetworkNeedsToken))
        }
        let certificate = !(server?["tls"]?["certificate"]?.string ?? "").isEmpty, key = !(server?["tls"]?["key"]?.string ?? "").isEmpty
        if certificate != key { issues.append(SchemaWalker.error("server.tls", .configTlsNeedsBoth)) }
        return issues
    }
}
