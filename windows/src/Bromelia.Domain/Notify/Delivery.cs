using Bromelia.Foundation;

namespace Bromelia.Domain;

/// <summary>How a notification goes out: an HTTP request, or an Apprise URL sent with the apprise command.</summary>
public abstract record Delivery
{
    public sealed record Http(HttpRequestSpec Request) : Delivery;
    public sealed record Apprise(string Url) : Delivery;
}
