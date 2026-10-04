using Bromelia.Domain;

namespace Bromelia.Adapters;

/// <summary>An info run: the listing it printed and the run.</summary>
public sealed record ListingRun(
    Listing Listing,
    MakemkvRun Run);
