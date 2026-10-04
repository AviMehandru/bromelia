using System.Collections.Generic;
using System.Text;
using System.Threading.Tasks;
using Bromelia.Domain;
using Bromelia.Foundation;
using Bromelia.Ports;
using CancellationToken = Bromelia.Foundation.CancellationToken;

namespace Bromelia.Adapters;

/// <summary>MakeMKV's free beta key (plan §10.2; shared/fixtures/adapters/beta-key-source.cases.json): the forum post
/// (BetaKeyPage.Url), read with BetaKeyPage.Parse. makemkv.betaKey.http for another status, .notFound when the page
/// has no key, .fetchFailed (with the reason) when there is no answer.</summary>
public sealed class BetaKeySource
{
    readonly IHttpClient _http;

    public BetaKeySource(IHttpClient http) { _http = http; }

    public async Task<string> CurrentKey(CancellationToken cancel)
    {
        HttpResponse response;
        try
        {
            response = await _http.Send(new HttpRequestSpec("GET", BetaKeyPage.Url, new List<KeyValuePair<string, string>>()), cancel).ConfigureAwait(false);
        }
        catch (BroFailure f) when (f.Error.Code == MessageCode.Wire(MessageCode.HttpFailed))
        {
            var reason = JsonValue.Of(("code", JsonValue.Of(f.Error.Code)), ("params", f.Error.Params));
            throw new BroFailure(new BroMessage(MessageCode.MakemkvBetaKeyFetchFailed, Severity.Error, ("reason", reason)).ToError());
        }
        if (response.Status != 200)
            throw new BroFailure(new BroMessage(MessageCode.MakemkvBetaKeyHttp, Severity.Error, ("status", JsonValue.Of(response.Status))).ToError());
        return BetaKeyPage.Parse(Encoding.UTF8.GetString(response.Body))
               ?? throw new BroFailure(new BroMessage(MessageCode.MakemkvBetaKeyNotFound, Severity.Error).ToError());
    }
}
