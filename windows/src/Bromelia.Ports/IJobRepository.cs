using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>The jobs table.</summary>
public interface IJobRepository
{
    Task Insert(JobRecord job);

    Task Update(JobRecord job);

    Task<JobRecord?> Load(Id jobId);

    /// <summary>Jobs that haven't finished, by queue and position.</summary>
    Task<IReadOnlyList<JobRecord>> Active();

    Task<IReadOnlyList<JobRecord>> Query(JobQuery query);
}
