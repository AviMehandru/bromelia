using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>Walks the compiled configuration schema (<see cref="ConfigSchema"/>): fills defaults, orders keys,
/// drops unknown keys and checks values. The same algorithm as tools/check-contracts.py's expand_defaults, so
/// every platform decodes a document the same way.</summary>
internal static class SchemaWalker
{
    private static readonly JsonValue Root = JsonValue.Parse(ConfigSchema.Text) ?? throw new InvalidOperationException("ConfigSchema isn't JSON");

    public static JsonValue Document => Root;

    public static JsonValue Def(string name) => Root["$defs"]![name]!;

    /// <summary>Follows $ref.</summary>
    public static JsonValue Resolve(JsonValue node)
    {
        for (int i = 0; i < 20 && node["$ref"]?.AsString is { } r; i++) node = Def(r.Substring("#/$defs/".Length));
        return node;
    }

    /// <summary>The object-typed branch of a node (through $ref, allOf, oneOf, anyOf), or null.</summary>
    public static JsonValue? ObjectVariant(JsonValue node)
    {
        node = Resolve(node);
        if (node["properties"] != null) return node;
        foreach (var key in new[] { "allOf", "oneOf", "anyOf" })
            foreach (var sub in node[key]?.AsArray ?? new List<JsonValue>())
                if (ObjectVariant(sub) is { } found) return found;
        return null;
    }

    public static JsonValue? ArrayItems(JsonValue node)
    {
        node = Resolve(node);
        if (node["items"] is { } items) return items;
        foreach (var key in new[] { "oneOf", "anyOf", "allOf" })
            foreach (var sub in node[key]?.AsArray ?? new List<JsonValue>())
                if (ArrayItems(sub) is { } found) return found;
        return null;
    }

    /// <summary>The additionalProperties schema of a map (MakemkvSettings, environment), or null.</summary>
    private static JsonValue? MapValues(JsonValue node)
    {
        node = Resolve(node);
        if (node["additionalProperties"] is JsonValue.Object o) return o;
        foreach (var key in new[] { "allOf", "oneOf", "anyOf" })
            foreach (var sub in node[key]?.AsArray ?? new List<JsonValue>())
                if (MapValues(sub) is { } found) return found;
        return null;
    }

    private static bool Inherits(JsonValue node)
    {
        node = Resolve(node);
        if (node["x-inherit"]?.AsBool == true) return true;
        return (node["allOf"]?.AsArray ?? new List<JsonValue>()).Any(Inherits);
    }

    private static bool IsObject(JsonValue node)
    {
        node = Resolve(node);
        return node["type"]?.AsString == "object" || node["properties"] != null;
    }

    private static bool Nullable(JsonValue node)
    {
        node = Resolve(node);
        var t = node["type"];
        if (t?.AsString == "null" || (t?.AsArray?.Any(x => x.AsString == "null") ?? false)) return true;
        return (node["oneOf"]?.AsArray ?? new List<JsonValue>()).Concat(node["anyOf"]?.AsArray ?? new List<JsonValue>()).Any(Nullable);
    }

    /// <summary>Whether the schema forbids <paramref name="key"/> in this object (allOf: if … then not required),
    /// so it mustn't be filled: a command step gets no handbrake object.</summary>
    private static bool Forbidden(JsonValue obj, string key, JsonValue value)
    {
        foreach (var sub in obj["allOf"]?.AsArray ?? new List<JsonValue>())
        {
            if (sub["if"] is not { } cond || sub["then"]?["not"]?["required"]?.AsArray is not { } required) continue;
            if (!required.Any(r => r.AsString == key)) continue;
            var l = new List<Issue>();
            Validate(value, cond, "", l);
            if (l.Count == 0) return true;
        }
        return false;
    }

    private static JsonValue? DefaultOf(JsonValue prop)
    {
        if (prop["default"] is { } d) return d;
        return Resolve(prop)["default"];
    }

    /// <summary>The value with the schema's keys in order, unknown keys dropped (each reported), and, unless
    /// <paramref name="fill"/> is false or the node is sparse (x-inherit), missing keys filled with their defaults
    /// (an object without a default becomes {} and is filled in turn).</summary>
    public static JsonValue Normalize(JsonValue value, JsonValue node, string path, bool fill, List<Issue> issues)
    {
        if (value is JsonValue.Object o)
        {
            if (ObjectVariant(node) is { } obj)
            {
                bool f = fill && !Inherits(node) && !Inherits(obj);
                var props = obj["properties"]!.AsObject!;
                var members = new List<KeyValuePair<string, JsonValue>>();
                var childIssues = new Dictionary<string, List<Issue>>();
                foreach (var (key, prop) in props.Select(p => (p.Key, p.Value)))
                {
                    var child = o[key];
                    if (child == null)
                    {
                        if (!f || Forbidden(obj, key, value)) continue;
                        if (DefaultOf(prop) is { } d) child = d;
                        // A step's background defaults by its kind: true for handbrake, false for command.
                        else if (key == "background" && obj["x-default-background"]?.AsBool == true) child = JsonValue.Of(o["kind"]?.AsString == "handbrake");
                        else if (IsObject(prop) && !Nullable(prop)) child = JsonValue.Of();
                        else continue;
                    }
                    var list = childIssues[key] = new List<Issue>();
                    members.Add(new(key, Normalize(child, prop, Join(path, key), f, list)));
                }
                // Issues in document order: an unknown key where it was written, a known key's own issues there too.
                foreach (var m in o.Members)
                {
                    if (childIssues.TryGetValue(m.Key, out var list)) issues.AddRange(list);
                    else if (!props.Any(p => p.Key == m.Key))
                        issues.Add(new Issue(Join(path, m.Key), MessageCode.ConfigUnknownKey, (JsonValue.Object)JsonValue.Of(), Severity.Warning));
                }
                return new JsonValue.Object(members);
            }
            if (MapValues(node) is { } values)
                return new JsonValue.Object(o.Members.Select(m => new KeyValuePair<string, JsonValue>(m.Key, Normalize(m.Value, values, Join(path, m.Key), fill, issues))).ToList());
            return value;
        }
        if (value is JsonValue.Array a && ArrayItems(node) is { } items)
            return new JsonValue.Array(a.Items.Select((v, i) => Normalize(v, items, path + "[" + i.ToString(CultureInfo.InvariantCulture) + "]", fill, issues)).ToList());
        return value;
    }

    public static string Join(string path, string key) => path.Length == 0 ? key : path + "." + key;

    // ---- validation ------------------------------------------------------------------------------------

    /// <summary>The issues of a value against a node: type, const, enum, required, minimum / maximum, pattern,
    /// minProperties, the branches of oneOf / anyOf, allOf, if / then and not. A SecretRef position holding text
    /// gives config.secretInline.</summary>
    public static void Validate(JsonValue value, JsonValue node, string path, List<Issue> issues)
    {
        node = Resolve(node);
        var branches = (node["oneOf"]?.AsArray ?? new List<JsonValue>()).Concat(node["anyOf"]?.AsArray ?? new List<JsonValue>()).ToList();
        if (branches.Count > 0)
        {
            var results = branches.Select(b => { var l = new List<Issue>(); Validate(value, b, path, l); return (Branch: b, Issues: l); }).ToList();
            if (!results.Any(r => r.Issues.Count == 0))
            {
                if (value is JsonValue.String && branches.Any(b => b["$ref"]?.AsString == "#/$defs/SecretRef"))
                    issues.Add(Error(path, MessageCode.ConfigSecretInline));
                else
                {
                    var typed = results.FirstOrDefault(r => TypeMatches(value, Resolve(r.Branch)));
                    if (typed.Issues != null) issues.AddRange(typed.Issues);
                    else issues.Add(Invalid(path, value));
                }
            }
        }
        foreach (var sub in node["allOf"]?.AsArray ?? new List<JsonValue>())
        {
            if (sub["if"] is { } cond)
            {
                var l = new List<Issue>();
                Validate(value, cond, path, l);
                if (l.Count == 0 && sub["then"] is { } then) Validate(value, then, path, issues);
                continue;
            }
            Validate(value, sub, path, issues);
        }
        if (node["not"] is { } not)
        {
            var l = new List<Issue>();
            Validate(value, not, path, l);
            if (l.Count == 0) issues.Add(Invalid(path, value));
        }
        if (!TypeMatches(value, node)) { issues.Add(Invalid(path, value)); return; }
        if (node["const"] is { } c && !c.Equals(value)) { issues.Add(Invalid(path, value)); return; }
        if (node["enum"]?.AsArray is { } e && !e.Any(x => x.Equals(value))) { issues.Add(Invalid(path, value)); return; }
        switch (value)
        {
            case JsonValue.Object o:
                foreach (var r in node["required"]?.AsArray ?? new List<JsonValue>())
                    if (o[r.AsString!] == null) issues.Add(Error(Join(path, r.AsString!), MessageCode.ConfigRequired));
                if (node["minProperties"]?.AsInteger is { } min && o.Members.Count < min) issues.Add(Invalid(path, value));
                var props = node["properties"];
                foreach (var m in o.Members)
                {
                    if (props?[m.Key] is { } prop) Validate(m.Value, prop, Join(path, m.Key), issues);
                    else if (node["additionalProperties"] is JsonValue.Object ap) Validate(m.Value, ap, Join(path, m.Key), issues);
                }
                break;
            case JsonValue.Array a:
                if (node["items"] is { } items)
                    for (int i = 0; i < a.Items.Count; i++) Validate(a.Items[i], items, path + "[" + i.ToString(CultureInfo.InvariantCulture) + "]", issues);
                break;
            case JsonValue.Integer or JsonValue.Number:
                var n = value.AsNumber!.Value;
                if ((node["minimum"]?.AsNumber is { } lo && n < lo) || (node["maximum"]?.AsNumber is { } hi && n > hi)) issues.Add(Invalid(path, value));
                break;
            case JsonValue.String s:
                if (node["pattern"]?.AsString is { } pattern && !Regex.IsMatch(s.Value, pattern, RegexOptions.CultureInvariant)) issues.Add(Invalid(path, value));
                break;
        }
    }

    private static bool TypeMatches(JsonValue value, JsonValue node)
    {
        var t = node["type"];
        if (t == null) return true;
        var types = t.AsArray?.Select(x => x.AsString!).ToList() ?? new List<string> { t.AsString! };
        return types.Any(type => type switch
        {
            "object" => value is JsonValue.Object,
            "array" => value is JsonValue.Array,
            "string" => value is JsonValue.String,
            "boolean" => value is JsonValue.Bool,
            "integer" => value is JsonValue.Integer,
            "number" => value is JsonValue.Integer or JsonValue.Number,
            "null" => value is JsonValue.Null,
            _ => false,
        });
    }

    public static Issue Error(string path, MessageCode code, params (string, JsonValue)[] p) =>
        new(path, code, (JsonValue.Object)JsonValue.Of(p), Severity.Error);

    public static Issue Invalid(string path, JsonValue value) =>
        Error(path, MessageCode.ConfigInvalidValue, ("value", JsonValue.Of(value is JsonValue.String s ? s.Value : value.ToString().TrimEnd('\n'))));
}
