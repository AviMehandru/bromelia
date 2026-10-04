using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>bromeliad as a login item / service.</summary>
public interface IServiceInstaller
{
    void Install();

    void Uninstall();

    bool IsInstalled();

    void Start();
}
