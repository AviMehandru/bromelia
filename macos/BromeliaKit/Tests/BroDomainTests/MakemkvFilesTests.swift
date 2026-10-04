import BroDomain
import BroFoundation
import BroTestSupport
import Foundation
import Testing

private func map(_ v: JsonValue?) -> [String: String] {
    Dictionary((v?.members ?? []).map { ($0.key, $0.value.string ?? "") }, uniquingKeysWith: { $1 })
}

private func mapJson(_ d: [String: String]) -> JsonValue {
    .object(d.sorted { $0.key.unicodeScalars.lexicographicallyPrecedes($1.key.unicodeScalars) }.map { ($0.key, .string($0.value)) })
}

private func sourceOf(_ s: JsonValue) -> MakemkvSource {
    if let d = s["drive"] { return .drive(index: Int(d["index"]?.int ?? 0), device: d["device"]?.string ?? "") }
    if let iso = s["iso"]?.string { return .iso(path: iso) }
    return .file(path: s["folder"]?.string ?? "")
}

/// Whether `xml` parses.
private func wellFormed(_ xml: String) -> Bool { XMLParser(data: Data(xml.utf8)).parse() }

struct MakemkvFilesTests {
    @Test func makemkvFilesCases() throws {
        let failures = try Fixtures.runCases("domain/makemkv-files.cases.json") { _, given, expect in
            if let text = given["text"]?.string {
                let parsed = SettingsConf.parse(text)
                try Fixtures.same(Fixtures.sorted(expect["parse"]!), mapJson(parsed), "parse")
                if expect["roundTrips"]?.bool == true {
                    try Fixtures.same(mapJson(parsed), mapJson(SettingsConf.parse(SettingsConf.render(parsed, header: "Test"))), "round trip")
                }
                return true
            }
            if let generated = given["generated"] {
                let xml = ProfileXml.render(generated)
                if expect["wellFormedXml"]?.bool == true { try Fixtures.check(wellFormed(xml), "well-formed XML") }
                for c in expect["contains"]?.array ?? [] { try Fixtures.check(xml.contains(c.string ?? ""), "contains \(c)") }
                if expect["containsMakemkvDefaultSelection"]?.bool == true {
                    try Fixtures.check(xml.contains("app_DefaultSelectionString=\"\(ProfileXml.makemkvDefaultSelection)\""), "default selection")
                }
                return true
            }
            if let global = given["global"] {
                let merged = SettingsLayers.merge(map(global), profile: map(given["profile"]), drive: map(given["drive"]),
                                                  registrationKey: given["registrationKey"]?.string, selectionOverride: given["selectionOverride"]?.string,
                                                  dataDir: given["makemkvDataDir"]?.string)
                try Fixtures.same(Fixtures.sorted(expect["settings"]!), mapJson(merged), "settings")
                return true
            }
            if let command = given["command"]?.string {
                let o = given["options"]
                let options = MakemkvOptions(profilePath: given["profilePath"]?.string, minLengthSeconds: o?["minLengthSeconds"]?.int.map(Int.init),
                                             cacheMB: o?["cacheMB"]?.int.map(Int.init), directIO: o?["directIO"]?.bool)
                let args: [String]
                do throws(BroError) {
                    switch command {
                    case "info": args = MakemkvArgs.info(sourceOf(given["source"]!), options: options)
                    case "mkv": args = MakemkvArgs.mkv(sourceOf(given["source"]!), title: given["title"]?.string ?? "", destination: given["destination"]?.string ?? "",
                                                       options: options)
                    case "backup": args = try MakemkvArgs.backup(sourceOf(given["source"]!), decrypt: given["decrypt"]?.bool ?? false,
                                                                 destination: given["destination"]?.string ?? "", options: options)
                    default: args = MakemkvArgs.scanDrives()
                    }
                } catch {
                    try Fixtures.same(expect["error"]?["code"]?.string, error.code, "error")
                    return true
                }
                try Fixtures.check(expect["error"] == nil, "expected an error")
                if let exact = expect["arguments"] { try Fixtures.same(exact, .array(args.map { .string($0) }), "arguments") }
                if let end = expect["endsWith"]?.array { try Fixtures.same(JsonValue.array(end), .array(args.suffix(end.count).map { .string($0) }), "endsWith") }
                for c in expect["contains"]?.array ?? [] { try Fixtures.check(args.contains(c.string ?? ""), "contains \(c)") }
                return true
            }
            if let path = given["path"]?.string {
                let source = SourceResolver.source(path, isDirectory: given["isDirectory"]?.bool ?? false)
                try Fixtures.same(expect["source"]?.string, MakemkvArgs.info(source, options: MakemkvOptions()).last, "source")
                return true
            }
            if given["html"] != nil || given["htmlFile"] != nil {
                let html = try given["html"]?.string ?? Fixtures.text(given["htmlFile"]?.string ?? "")
                try Fixtures.same(expect["key"]?.string, BetaKeyPage.parse(html), "key")
                return true
            }
            if (given.members ?? []).contains(where: { $0.key == "currentKey" }) {
                try Fixtures.same(expect["mayReplace"]?.bool, BetaKey.mayReplace(given["currentKey"]?.string), "mayReplace")
                return true
            }
            return false
        }
        #expect(failures.isEmpty, "\(failures.joined(separator: "\n"))")
    }
}
