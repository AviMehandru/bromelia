using Bromelia.Foundation;

namespace Bromelia.Ports;

/// <summary>New ids for what the engine creates (jobs first): randomness is I/O, so it comes through a port. The
/// adapter gives random UUIDs; the test kit counts.</summary>
public interface IIdGenerator
{
    /// <summary>An id no other call has given: a lower-case UUID.</summary>
    Id Next();
}
