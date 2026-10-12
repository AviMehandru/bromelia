using Bromelia.Domain;
using Bromelia.Foundation;
using Xunit;
using static Bromelia.Tests.Fixtures;

namespace Bromelia.Domain.Tests;

/// <summary>shared/fixtures/domain/job-details.cases.json.</summary>
public class JobDetailsTests
{
    internal static JobRequest RequestOf(JsonValue r) => new(
        EnumWire.Parse<JobKind>(r["kind"]?.AsString) ?? JobKind.VideoDisc, r["title"]?.AsString ?? "", r["automatic"]?.AsBool ?? false,
        r["driveId"]?.AsString, r["mediaGeneration"]?.AsInteger, r["sessionId"]?.AsString is { } s ? new Id(s) : null,
        Options: r["options"], Details: r["details"]);

    static JsonValue Encoded(DiscRequest? r) => r is null ? JsonValue.Null.Instance : DiscRequest.Encode(r);

    [Fact]
    public void TheSharedCasesPass() => RunCases("domain/job-details.cases.json", (id, given, expect) =>
    {
        if (given["request"] is { } request)
            Same(expect["discRequest"], Encoded(DiscRequest.Decode(RequestOf(request))), "discRequest");
        else if (given["requests"] is { } requests)
            Same(expect["discRequests"], JsonValue.Of(requests.AsArray!.Select(r => Encoded(DiscRequest.Decode(RequestOf(r))))), "discRequests");
        else if (given["choices"] is { } choices)
            Same(expect["choices"], SessionChoices.Encode(SessionChoices.Decode(choices)), "choices");
        else if (given["identity"] is { } identity)
            Same(expect["identity"], Identity.Encode(Identity.Decode(identity)), "identity");
        else if (given["resolve"] is { } resolve)
        {
            var listing = ListingTests.ListingOf(GeneratedListing(resolve["listing"]!));
            var resolved = Identity.Resolve(new IdentityInputs(listing, listing.VolumeName, false));
            Same(expect["identity"], Identity.Encode(resolved), "identity");
            Same(expect["identity"], Identity.Encode(Identity.Decode(Identity.Encode(resolved))), "identity read back");
        }
        else if (given["message"] is { } message)
        {
            var m = BroMessage.Decode(message);
            Same(expect["message"], m.ToJson(), "message");
            Same(expect["severity"]!.AsString, EnumWire.Name(m.Severity), "severity");
        }
        else if (given["automationOf"] is { } of)
        {
            var config = ConfigTests.Decode(given["config"]!);
            Same(expect["automation"], JsonValue.Of(of.AsArray!.Select(e => Config.Automation(config, e.AsString))), "automation");
        }
        else if (given["outputOf"] is { } kinds)
        {
            var outputs = given["outputs"]!.AsArray!.Select(o => new StepOutput(EnumWire.Parse<StepKind>(o["kind"]!.AsString)!.Value, o["data"]!)).ToList();
            var job = new JobSnapshot(new Id("00000000-0000-4000-8000-000000000001"), new JobRequest(JobKind.VideoDisc, ""), null, outputs);
            Same(expect["outputs"], JsonValue.Of(kinds.AsArray!.Select(k => JobSnapshot.Output(job, EnumWire.Parse<StepKind>(k.AsString)!.Value)
                ?? JsonValue.Null.Instance)), "outputs");
        }
        else return false;
        return true;
    });
}
