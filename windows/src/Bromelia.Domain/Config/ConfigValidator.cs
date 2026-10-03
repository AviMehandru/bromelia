using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Every problem of a configuration, with its JSON path: the issues decoding found (unknown keys), values
/// the schema doesn't allow (config.invalidValue, config.required; config.secretInline for a secret written into
/// the document), regular expressions that don't compile, references to entries that don't exist, ids used twice,
/// and the server's rules (a network address needs a token; TLS needs both files).</summary>
public static class ConfigValidator
{
    public static List<Issue> Validate(Config config)
    {
        var issues = new List<Issue>(config.Issues);
        var doc = config.Document;
        SchemaWalker.Validate(doc, SchemaWalker.Document, "", issues);
        string At(string list, int i, string rest) => list + "[" + i.ToString(CultureInfo.InvariantCulture) + "]" + (rest.Length > 0 ? "." + rest : "");

        void Regex(JsonValue? v, string path)
        {
            if (v?.AsString is not { Length: > 0 } pattern) return;
            try { _ = new Regex(pattern, RegexOptions.IgnoreCase); }
            catch (ArgumentException) { issues.Add(SchemaWalker.Error(path, MessageCode.ConfigInvalidRegex)); }
        }
        void Titles(JsonValue? profile, string path)
        {
            var t = profile?["titles"];
            Regex(t?["includePattern"], path + ".titles.includePattern");
            Regex(t?["excludePattern"], path + ".titles.excludePattern");
        }

        var ids = new Dictionary<string, HashSet<string>>();
        foreach (var kind in new[] { "libraries", "profiles", "drives", "steps", "targets", "rules" })
        {
            var seen = new HashSet<string>();
            var list = doc[kind]?.AsArray ?? new List<JsonValue>();
            for (int i = 0; i < list.Count; i++)
                if (list[i]["id"]?.AsString is { } id && !seen.Add(id))
                    issues.Add(SchemaWalker.Error(At(kind, i, "id"), MessageCode.ConfigDuplicateId, ("id", JsonValue.Of(id))));
            ids[kind] = seen;
        }
        void Need(string kind, string noun, JsonValue? v, string path)
        {
            if (v?.AsString is { } id && !ids[kind].Contains(id))
                issues.Add(SchemaWalker.Error(path, MessageCode.ConfigUnknownReference, ("kind", JsonValue.Of(noun)), ("id", JsonValue.Of(id))));
        }
        void NeedAll(string kind, string noun, JsonValue? list, string path)
        {
            var items = list?.AsArray ?? new List<JsonValue>();
            for (int i = 0; i < items.Count; i++) Need(kind, noun, items[i], path + "[" + i.ToString(CultureInfo.InvariantCulture) + "]");
        }

        Need("libraries", "library", doc["defaultLibrary"], "defaultLibrary");
        Need("profiles", "profile", doc["defaultProfile"], "defaultProfile");
        var profiles = doc["profiles"]?.AsArray ?? new List<JsonValue>();
        for (int i = 0; i < profiles.Count; i++)
        {
            Need("libraries", "library", profiles[i]["library"], At("profiles", i, "library"));
            NeedAll("steps", "step", profiles[i]["steps"], At("profiles", i, "steps"));
            NeedAll("targets", "target", profiles[i]["archive"]?["replicateTo"], At("profiles", i, "archive.replicateTo"));
            Titles(profiles[i], At("profiles", i, ""));
        }
        var drives = doc["drives"]?.AsArray ?? new List<JsonValue>();
        for (int i = 0; i < drives.Count; i++) Need("profiles", "profile", drives[i]["profile"], At("drives", i, "profile"));
        var rules = doc["rules"]?.AsArray ?? new List<JsonValue>();
        for (int i = 0; i < rules.Count; i++)
        {
            var when = rules[i]["when"];
            foreach (var key in new[] { "nameOrLabel", "name", "label" }) Regex(when?[key], At("rules", i, "when." + key));
            NeedAll("profiles", "profile", when?["profiles"], At("rules", i, "when.profiles"));
            NeedAll("drives", "drive", when?["drives"], At("rules", i, "when.drives"));
            Need("profiles", "profile", rules[i]["then"]?["profile"], At("rules", i, "then.profile"));
            NeedAll("steps", "step", rules[i]["then"]?["steps"], At("rules", i, "then.steps"));
            Titles(rules[i]["then"]?["set"], At("rules", i, "then.set"));
        }

        var server = doc["server"];
        var tcp = server?["tcp"];
        if (tcp?["enabled"]?.AsBool == true && tcp["address"]?.AsString is { } address && address != "127.0.0.1"
            && (server?["tokens"]?.AsArray?.Count ?? 0) == 0)
            issues.Add(SchemaWalker.Error("server.tcp.address", MessageCode.ConfigNetworkNeedsToken));
        var tls = server?["tls"];
        if ((tls?["certificate"]?.AsString?.Length > 0) != (tls?["key"]?.AsString?.Length > 0))
            issues.Add(SchemaWalker.Error("server.tls", MessageCode.ConfigTlsNeedsBoth));
        return issues;
    }
}
