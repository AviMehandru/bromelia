using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>A file read as a stream (hashing).</summary>
public interface IByteStream
{
    /// <summary>Up to maxBytes; empty at the end.</summary>
    byte[] Read(int maxBytes);

    /// <summary>Closes it.</summary>
    void Close();
}
