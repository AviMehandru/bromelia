namespace Bromelia.Foundation;

/// <summary>A UUID in lower-case text. Ids are made by the engine's adapters (randomness is I/O).</summary>
public readonly record struct Id(string Value)
{
    /// <summary>The first eight hex digits, as used in file names (<c>bromelia-&lt;unit8&gt;.json</c>).</summary>
    public static string Short(Id id)
    {
        var hex = id.Value.Replace("-", "").ToLowerInvariant();
        return hex.Length <= 8 ? hex : hex.Substring(0, 8);
    }

    public override string ToString() => Value;
}
