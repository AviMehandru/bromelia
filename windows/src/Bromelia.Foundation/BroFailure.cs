using System;

namespace Bromelia.Foundation;

/// <summary>The one exception that crosses layers (plan §6): it carries a <see cref="BroError"/>.</summary>
public sealed class BroFailure : Exception
{
    public BroError Error { get; }

    public BroFailure(BroError error) : base(error.Code) { Error = error; }
}
