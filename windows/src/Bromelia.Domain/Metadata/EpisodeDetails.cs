namespace Bromelia.Domain;

/// <summary>An episode of a season, as listed online: its title, air date (yyyy-mm-dd or "") and plot.</summary>
public sealed record EpisodeDetails(string Title, string Aired = "", string Plot = "");
