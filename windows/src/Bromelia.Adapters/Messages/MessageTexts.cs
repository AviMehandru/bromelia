using System;
using System.Collections.Generic;

namespace Bromelia.Adapters;

/// <summary>One language's message texts (ICU MessageFormat, shared/messages/&lt;lang&gt;.json) by code, and the params
/// codes.json declares as messageList (rendered joined with ", "; other message lists with "; ").</summary>
public sealed class MessageTexts
{
    private readonly IReadOnlyDictionary<string, string> _texts;
    private readonly HashSet<(string, string)> _messageLists;

    public MessageTexts(IReadOnlyDictionary<string, string> texts, IEnumerable<(string Code, string Param)> messageLists)
    {
        _texts = texts;
        _messageLists = new HashSet<(string, string)>(messageLists);
    }

    /// <summary>The text of a code; null when the catalog has none.</summary>
    public string? Text(string code) => _texts.TryGetValue(code, out var text) ? text : null;

    /// <summary>Whether a code's param is a messageList.</summary>
    public bool IsMessageList(string code, string param) => _messageLists.Contains((code, param));
}
