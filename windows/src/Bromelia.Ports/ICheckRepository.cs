using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>The checks table.</summary>
public interface ICheckRepository
{
    Task Insert(CheckRecord check);

    /// <summary>Newest first.</summary>
    Task<IReadOnlyList<CheckRecord>> ForUnit(Id unitId);

    Task<CheckRecord?> Latest(string folder);
}
