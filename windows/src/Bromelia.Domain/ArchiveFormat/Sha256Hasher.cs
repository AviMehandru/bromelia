using System;
using System.Security.Cryptography;

namespace Bromelia.Domain;

/// <summary>SHA-256 fed a piece at a time (a file read back from disk as a stream), lower-case hex at the end.</summary>
public sealed class Sha256Hasher
{
    readonly IncrementalHash _hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);

    public void Update(byte[] bytes) => _hash.AppendData(bytes);

    /// <summary>The digest, lower-case hex; the hasher is spent.</summary>
    public string Finish()
    {
        var hex = Convert.ToHexString(_hash.GetHashAndReset()).ToLowerInvariant();
        _hash.Dispose();
        return hex;
    }
}
