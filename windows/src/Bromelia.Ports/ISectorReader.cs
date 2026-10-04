using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Raw reads of a disc (data discs, rescue).</summary>
public interface ISectorReader
{
    /// <summary>count 2048-byte sectors from sector; fails on a read error.</summary>
    byte[] Read(long sector, int count);

    long SectorCount();

    void Close();
}
