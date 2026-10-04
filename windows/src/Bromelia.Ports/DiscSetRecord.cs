using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A row of disc_sets: the discs of a release that belong together.</summary>
public sealed record DiscSetRecord(
    Id Id,
    Id? WorkId,
    string LabelTitle,
    int? Season,
    int? Part,
    int? Volume,
    string Description,
    int? KnownCount = null);
