using System.Text.RegularExpressions;
using Bromelia.Core.Config;
using Bromelia.Core.Robot;

namespace Bromelia.Core.Logic;

/// <summary>Index patterns such as <c>0,2-4,7-</c>, <c>last</c>, <c>all</c> or <c>*</c>.</summary>
public sealed class IndexPattern
{
    abstract record Item;
    sealed record Single(int N) : Item;
    sealed record Range(int Low, int? High) : Item;
    sealed record Last : Item;
    sealed record All : Item;

    readonly List<Item> _items = new();

    public IndexPattern(string text)
    {
        foreach (var raw in text.Split(new[] { ',', ';', ' ' }, StringSplitOptions.RemoveEmptyEntries))
        {
            var part = raw.Trim().ToLowerInvariant();
            if (part is "*" or "all") { _items.Add(new All()); continue; }
            if (part == "last") { _items.Add(new Last()); continue; }
            int dash = part.IndexOf('-');
            if (dash >= 0)
            {
                var lo = part[..dash];
                var hi = part[(dash + 1)..];
                if (!int.TryParse(lo, out var l)) throw new FormatException($"Invalid index pattern element “{part}”");
                if (hi.Length == 0) { _items.Add(new Range(l, null)); continue; }
                if (!int.TryParse(hi, out var h) || h < l) throw new FormatException($"Invalid index pattern element “{part}”");
                _items.Add(new Range(l, h));
            }
            else
            {
                if (!int.TryParse(part, out var n) || n < 0) throw new FormatException($"Invalid index pattern element “{part}”");
                _items.Add(new Single(n));
            }
        }
    }

    public bool Matches(int value, int maxValue) => _items.Any(i => i switch
    {
        All => true,
        Last => value == maxValue,
        Single s => s.N == value,
        Range r => value >= r.Low && (r.High == null || value <= r.High),
        _ => false,
    });
}

public sealed record TitleDecision(int TitleIndex, bool Selected, string Reason);

public sealed class TitleSelectionResult
{
    public List<TitleDecision> Decisions { get; init; } = new();
    public bool RequiresManualChoice { get; init; }
    public string? Error { get; init; }
    public List<int> SelectedIndices => Decisions.Where(d => d.Selected).Select(d => d.TitleIndex).OrderBy(i => i).ToList();
}

public static class TitleSelector
{
    public static TitleSelectionResult Evaluate(IReadOnlyList<TitleInfo> titles, TitleSelection rule)
    {
        var decisions = new Dictionary<int, TitleDecision>();
        void Reject(TitleInfo t, string why)
        {
            if (!decisions.ContainsKey(t.Index)) decisions[t.Index] = new TitleDecision(t.Index, false, why);
        }

        Regex? include = null, exclude = null;
        string? error = null;
        try { if (rule.IncludePattern.Length > 0) include = new Regex(rule.IncludePattern, RegexOptions.IgnoreCase); }
        catch (ArgumentException) { error = "Invalid include pattern"; }
        try { if (rule.ExcludePattern.Length > 0) exclude = new Regex(rule.ExcludePattern, RegexOptions.IgnoreCase); }
        catch (ArgumentException) { error = "Invalid exclude pattern"; }

        var candidates = new List<TitleInfo>();
        foreach (var t in titles.OrderBy(t => t.Index))
        {
            int d = t.DurationSeconds;
            long mb = t.SizeBytes / 1_000_000;
            if (rule.MinDurationSeconds > 0 && d < rule.MinDurationSeconds) { Reject(t, $"Shorter than {TitleInfo.FormatDuration(rule.MinDurationSeconds)}"); continue; }
            if (rule.MaxDurationSeconds > 0 && d > rule.MaxDurationSeconds) { Reject(t, $"Longer than {TitleInfo.FormatDuration(rule.MaxDurationSeconds)}"); continue; }
            if (rule.MinChapters > 0 && t.ChapterCount < rule.MinChapters) { Reject(t, $"Fewer than {rule.MinChapters} chapters"); continue; }
            if (rule.MaxChapters > 0 && t.ChapterCount > rule.MaxChapters) { Reject(t, $"More than {rule.MaxChapters} chapters"); continue; }
            if (rule.MinSizeMB > 0 && mb < rule.MinSizeMB) { Reject(t, $"Smaller than {rule.MinSizeMB} MB"); continue; }
            if (rule.MaxSizeMB > 0 && mb > rule.MaxSizeMB) { Reject(t, $"Larger than {rule.MaxSizeMB} MB"); continue; }
            var hay = MatchText(t);
            if (include != null && !include.IsMatch(hay)) { Reject(t, "Does not match include pattern"); continue; }
            if (exclude != null && exclude.IsMatch(hay)) { Reject(t, "Matches exclude pattern"); continue; }
            if (rule.SkipAlternateAngles && t.Angle is > 1) { Reject(t, $"Alternate angle {t.Angle}"); continue; }
            candidates.Add(t);
        }

        if (rule.SkipDuplicates)
        {
            var seen = new Dictionary<string, int>();
            candidates = candidates.Where(t =>
            {
                if (t.SegmentMap.Length == 0) return true;
                var key = $"{t.SegmentMap}|{t.DurationSeconds}|{t.Angle ?? 0}";
                if (seen.TryGetValue(key, out var first)) { Reject(t, $"Duplicate of title {first}"); return false; }
                seen[key] = t.Index;
                return true;
            }).ToList();
        }

        var chosen = new List<TitleInfo>();
        bool manual = false;
        switch (rule.Strategy)
        {
            case TitleStrategy.All:
                chosen = candidates;
                break;
            case TitleStrategy.Longest:
                int n = Math.Max(1, rule.LongestCount);
                var ranked = candidates.OrderByDescending(t => t.DurationSeconds).ThenByDescending(t => t.ChapterCount)
                    .ThenByDescending(t => t.SizeBytes).ThenBy(t => t.Index).ToList();
                chosen = ranked.Take(n).ToList();
                foreach (var t in ranked.Skip(n)) Reject(t, $"Not among the {n} longest");
                break;
            case TitleStrategy.Indices:
                try
                {
                    var pattern = new IndexPattern(rule.IndexPattern);
                    int max = rule.IndexBase == IndexBase.Makemkv
                        ? (titles.Count > 0 ? titles.Max(t => t.Index) : 0)
                        : titles.Select(t => t.SourceTitleId ?? 0).DefaultIfEmpty(0).Max();
                    foreach (var t in candidates)
                    {
                        int v = rule.IndexBase == IndexBase.Makemkv ? t.Index : t.SourceTitleId ?? -1;
                        if (pattern.Matches(v, max)) chosen.Add(t); else Reject(t, "Not in index pattern");
                    }
                }
                catch (FormatException e)
                {
                    error = e.Message;
                    foreach (var t in candidates) Reject(t, "Invalid index pattern");
                }
                break;
            case TitleStrategy.Manual:
                manual = true;
                foreach (var t in candidates) decisions[t.Index] = new TitleDecision(t.Index, false, "Choose manually");
                break;
        }

        chosen = chosen.OrderBy(t => t.Index).ToList();
        if (rule.MaxTitles > 0 && chosen.Count > rule.MaxTitles)
        {
            foreach (var t in chosen.Skip(rule.MaxTitles)) Reject(t, $"Over the limit of {rule.MaxTitles} titles");
            chosen = chosen.Take(rule.MaxTitles).ToList();
        }
        foreach (var t in chosen) decisions[t.Index] = new TitleDecision(t.Index, true, "Selected");

        return new TitleSelectionResult
        {
            Decisions = titles.Select(t => decisions.TryGetValue(t.Index, out var d) ? d : new TitleDecision(t.Index, false, "Excluded")).ToList(),
            RequiresManualChoice = manual,
            Error = error,
        };
    }

    public static string MatchText(TitleInfo t) =>
        string.Join(" ", new[] { t.Name, t.Comment, t.OutputFileName, t.SourceFileName, t.SegmentMap,
            t.SourceTitleId is { } id ? $"#{id}" : "", t.DurationText }.Where(s => s.Length > 0));
}
