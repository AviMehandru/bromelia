using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>The text of a message in the configured language (plan §16: shared/messages/en.json, rendered by the host's
/// adapter): for what the engine writes for people to read, such as the INCOMPLETE.txt note.</summary>
public interface IMessageRenderer
{
    string Render(BroMessage message);
}
