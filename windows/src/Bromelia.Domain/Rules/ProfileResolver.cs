using System.Collections.Generic;
using System.Linq;
using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>The effective profile of a disc (plan §24).</summary>
public static class ProfileResolver
{
    /// <summary>Layers, lowest first, each merged like an RFC 7386 merge patch (null removes a key, so its default
    /// comes back): every field at its default → the default profile → the drive's profile → the drive's own MakeMKV
    /// settings → each enabled rule that matches, in order (its then.set; then.profile replaces the drive's profile
    /// and later rules still apply) → the session's choices. Steps are the profile's, then the rules', each once.
    /// <paramref name="driveId"/> is the drive entry's id (null: no entry, or an ISO / folder).</summary>
    public static EffectiveProfile Resolve(Config config, string? driveId, RuleFacts facts, JsonValue? sessionChoices)
    {
        var doc = config.Document;
        var profiles = Config.Profiles(config);
        JsonValue? ProfileJson(string? id) => profiles.FirstOrDefault(p => p.Id == id)?.Json;
        var defaultId = doc["defaultProfile"]?.AsString ?? "default";
        var drive = driveId == null ? null : Config.Drives(config).FirstOrDefault(d => d.Id == driveId);
        string? driveProfileId = drive?.Profile is { } dp && dp != defaultId ? dp : null;

        var ruleLayers = new List<(Rule Rule, JsonValue? Set)>();
        var ruleSteps = new List<string>();
        var currentProfile = driveProfileId ?? defaultId;
        foreach (var rule in Config.Rules(config).Where(r => r.Enabled))
        {
            if (!RuleMatcher.Matches(rule.When, facts with { DriveId = driveId, ProfileId = currentProfile })) continue;
            if (rule.Then.Profile is { } switchTo)
            {
                driveProfileId = switchTo == defaultId ? null : switchTo;
                currentProfile = switchTo;
            }
            ruleLayers.Add((rule, rule.Then.Set));
            ruleSteps.AddRange(rule.Then.Steps);
        }

        var trace = new List<ResolveTrace>();
        var issues = new List<Issue>();
        var node = SchemaWalker.Def("ProfileFields");
        var profile = SchemaWalker.Normalize(JsonValue.Of(), node, "", true, issues, full: true);
        trace.Add(new ResolveTrace(ResolveLayer.Defaults, null, new List<string>()));
        void Layer(ResolveLayer layer, string? id, JsonValue? patch)
        {
            var p = Strip(patch);
            profile = MergePatch(profile, p);
            trace.Add(new ResolveTrace(layer, id, LeafKeys(p, "")));
        }
        Layer(ResolveLayer.DefaultProfile, defaultId, ProfileJson(defaultId));
        if (driveProfileId != null) Layer(ResolveLayer.DriveProfile, driveProfileId, ProfileJson(driveProfileId));
        if (drive != null)
        {
            // In the document's order (DriveEntry's dictionary has none).
            var settings = doc["drives"]!.AsArray!.First(d => d["id"]?.AsString == drive.Id)["makemkvSettings"];
            Layer(ResolveLayer.DriveOverrides, drive.Id, settings is JsonValue.Object { Members.Count: > 0 }
                ? JsonValue.Of(("makemkv", JsonValue.Of(("settings", settings)))) : JsonValue.Of());
        }
        foreach (var (rule, set) in ruleLayers) Layer(ResolveLayer.Rule, rule.Id, set);
        if (sessionChoices != null) Layer(ResolveLayer.Session, null, sessionChoices);
        // A key a patch removed takes its default again.
        profile = SchemaWalker.Normalize(profile, node, "", true, issues, full: true);

        var steps = new List<string>();
        foreach (var s in (profile["steps"]?.AsArray ?? new List<JsonValue>()).Select(v => v.AsString ?? "").Concat(ruleSteps))
            if (!steps.Contains(s)) steps.Add(s);
        return new EffectiveProfile(profile, steps, trace);
    }

    /// <summary>A profile as a patch: without its id and name.</summary>
    private static JsonValue Strip(JsonValue? p) => p is JsonValue.Object o
        ? new JsonValue.Object(o.Members.Where(m => m.Key is not ("id" or "name")).ToList())
        : JsonValue.Of();

    /// <summary>RFC 7386: objects merge key by key, null removes a key, anything else replaces.</summary>
    internal static JsonValue MergePatch(JsonValue target, JsonValue patch)
    {
        if (patch is not JsonValue.Object p) return patch;
        var members = (target as JsonValue.Object)?.Members.ToList() ?? new List<KeyValuePair<string, JsonValue>>();
        foreach (var m in p.Members)
        {
            int at = members.FindIndex(x => x.Key == m.Key);
            if (m.Value is JsonValue.Null)
            {
                if (at >= 0) members.RemoveAt(at);
                continue;
            }
            var merged = MergePatch(at >= 0 ? members[at].Value : JsonValue.Of(), m.Value);
            if (at >= 0) members[at] = new(m.Key, merged);
            else members.Add(new(m.Key, merged));
        }
        return new JsonValue.Object(members);
    }

    private static List<string> LeafKeys(JsonValue v, string prefix)
    {
        var keys = new List<string>();
        foreach (var m in v.AsObject ?? new List<KeyValuePair<string, JsonValue>>())
        {
            var path = prefix.Length == 0 ? m.Key : prefix + "." + m.Key;
            if (m.Value is JsonValue.Object o && o.Members.Count > 0) keys.AddRange(LeafKeys(m.Value, path));
            else keys.Add(path);
        }
        return keys;
    }
}
