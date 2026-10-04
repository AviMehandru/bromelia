using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>Engine-side notifications, used when no app is connected.</summary>
public interface IDesktopNotifier
{
    void Post(string title, string body, bool sound);
}
