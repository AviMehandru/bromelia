namespace Bromelia.Domain;

/// <summary>An opened disc as it was when the user ripped it: the listing it was opened with and the choices made on
/// it (plan §15.3). ReconcileStep checks the disc is still that disc.</summary>
public sealed record SessionSnapshot(Listing Listing, SessionChoices Choices);
