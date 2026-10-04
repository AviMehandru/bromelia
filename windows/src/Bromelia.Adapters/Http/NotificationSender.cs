using System;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>Notifications (plan §10.2; shared/fixtures/adapters/notification-sender.cases.json): NotifyRequests.build,
/// then the HttpClient or Apprise. A failure is notify.badUrl, notify.httpFailed {host, status}, notify.failed {host,
/// reason}, notify.needsApprise or notify.appriseFailed; none names the URL, which is a secret.</summary>
public sealed class NotificationSender
{
    readonly IHttpClient _http;
    readonly AppriseTool _apprise;

    public NotificationSender(IHttpClient http, AppriseTool apprise)
    {
        _http = http;
        _apprise = apprise;
    }

    public async Task Send(string url, string title, string body, StatusWord status, CancellationToken cancel)
    {
        switch (NotifyRequests.Build(url, title, body, status))
        {
            case Delivery.Http { Request: var request }:
                var host = Uri.TryCreate(request.Url, UriKind.Absolute, out var u) ? u.Host : "";
                HttpResponse response;
                try
                {
                    response = await _http.Send(request, cancel).ConfigureAwait(false);
                }
                catch (BroFailure f) when (f.Error.Code == MessageCode.Wire(MessageCode.HttpFailed))
                {
                    throw new BroFailure(new BroMessage(MessageCode.NotifyFailed, Severity.Warning, ("host", JsonValue.Of(host)),
                        ("reason", f.Error.Params["reason"] ?? JsonValue.Of(""))).ToError());
                }
                if (response.Status < 200 || response.Status >= 300)
                    throw new BroFailure(new BroMessage(MessageCode.NotifyHttpFailed, Severity.Warning, ("host", JsonValue.Of(host)),
                        ("status", JsonValue.Of(response.Status))).ToError());
                break;
            case Delivery.Apprise { Url: var appriseUrl }:
                await _apprise.Send(appriseUrl, title, body, cancel).ConfigureAwait(false);
                break;
            default:
                throw new BroFailure(new BroMessage(MessageCode.NotifyBadUrl, Severity.Warning, ("target", JsonValue.Of(AppriseTool.Scheme(url)))).ToError());
        }
    }
}
