using System.Text;

namespace IslandBridge.Core.Icons;

public sealed record InstalledApp(string Name, string ParsingName);

public static class AppNameMatcher
{
    public const int MaxNameLength = 128;

    public static InstalledApp? FindBest(IReadOnlyList<InstalledApp> apps, string? appName)
    {
        ArgumentNullException.ThrowIfNull(apps);

        var wanted = Normalize(appName);

        if (wanted.Length == 0)
        {
            return null;
        }

        foreach (var app in apps)
        {
            if (Normalize(app.Name) == wanted && !app.ParsingName.Contains("://", StringComparison.Ordinal))
            {
                return app;
            }
        }

        return null;
    }

    public static string Normalize(string? name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return string.Empty;
        }

        var text = name.Length > MaxNameLength ? name[..MaxNameLength] : name;
        var parenthesis = text.IndexOf('(', StringComparison.Ordinal);

        if (parenthesis > 0)
        {
            text = text[..parenthesis];
        }

        var builder = new StringBuilder(text.Length);

        foreach (var ch in text)
        {
            if (char.IsLetterOrDigit(ch))
            {
                builder.Append(char.ToLowerInvariant(ch));
            }
        }

        var normalized = builder.ToString();
        return normalized.StartsWith("microsoft", StringComparison.Ordinal) && normalized.Length > "microsoft".Length
            ? normalized["microsoft".Length..]
            : normalized;
    }
}
