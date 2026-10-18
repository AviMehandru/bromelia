using System;
using Bromelia.Foundation;
using Bromelia.Ports;

namespace Bromelia.Adapters;

/// <summary>Random (version 4) UUIDs in lower-case text.</summary>
public sealed class RandomIdGenerator : IIdGenerator
{
    public Id Next() => new(Guid.NewGuid().ToString("D"));
}
