using Bromelia.Domain;

namespace Bromelia.Adapters;

/// <summary>What a DataImager copy made: the bytes copied, and a warning when the image is complete but its folder
/// couldn't be flushed (fs.notSynced).</summary>
public sealed record DataImageCopy(long Bytes, BroMessage? Warning);
