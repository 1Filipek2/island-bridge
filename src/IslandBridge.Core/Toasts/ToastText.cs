using IslandBridge.Core.Ancs;
using IslandBridge.Core.Notifications;
using IslandBridge.Core.Text;

namespace IslandBridge.Core.Toasts;

public static class ToastText
{
    public const int MaxTextElements = 16;
    public const int MaxElementLength = 512;
    public const string FallbackAppName = "Windows";

    public static IslandNotification? Map(ToastDetails details, long receivedTimestamp)
    {
        ArgumentNullException.ThrowIfNull(details);

        var title = string.Empty;
        List<string> rest = [];

        foreach (var text in details.Texts.Take(MaxTextElements))
        {
            var clean = TextSanitizer.Sanitize(text ?? string.Empty, MaxElementLength);

            if (clean.Length == 0)
            {
                continue;
            }

            if (title.Length == 0)
            {
                title = clean;
            }
            else
            {
                rest.Add(clean);
            }
        }

        if (title.Length == 0)
        {
            return null;
        }

        var appIdentifier = TextSanitizer.Sanitize(details.AppUserModelId ?? string.Empty, AncsLimits.MaxAppIdentifierLength);
        var appName = TextSanitizer.Sanitize(details.DisplayName ?? string.Empty, AncsLimits.MaxDisplayNameLength);

        return new IslandNotification(
            details.Id,
            AncsCategory.Other,
            AncsEventFlags.None,
            appIdentifier,
            appName.Length > 0 ? appName : FallbackAppName,
            title,
            string.Empty,
            TextSanitizer.Sanitize(string.Join(' ', rest), MaxElementLength),
            details.CreatedAt.LocalDateTime,
            receivedTimestamp,
            NotificationOrigin.Windows);
    }
}
