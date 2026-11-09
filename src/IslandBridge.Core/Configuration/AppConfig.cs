using IslandBridge.Core.Ancs;

namespace IslandBridge.Core.Configuration;

public enum AppLanguage
{
    System,
    English,
    Slovak,
}

public enum AppTheme
{
    System,
    Light,
    Dark,
}

public enum CloseAction
{
    Tray,
    Exit,
}

public sealed class AppConfig
{
    public const int CurrentVersion = 1;
    public const int MaxMutedApps = 256;
    public const int MaxStoredAppNames = AppNameCache.DefaultCapacity;

    public static IReadOnlyList<int> AllowedStartupDelays { get; } = [0, 10, 30, 60];

    public int Version { get; set; } = CurrentVersion;

    public bool FirstRunCompleted { get; set; }

    public DeviceConfig Device { get; set; } = new();

    public StartupConfig Startup { get; set; } = new();

    public NotificationConfig Notifications { get; set; } = new();

    public AppearanceConfig Appearance { get; set; } = new();

    public UpdatesConfig Updates { get; set; } = new();

    public Dictionary<string, string> AppNames { get; set; } = new(StringComparer.Ordinal);

    public AppConfig Normalize()
    {
        Device ??= new DeviceConfig();
        Startup ??= new StartupConfig();
        Notifications ??= new NotificationConfig();
        Appearance ??= new AppearanceConfig();
        Updates ??= new UpdatesConfig();
        AppNames ??= new Dictionary<string, string>(StringComparer.Ordinal);

        if (string.IsNullOrWhiteSpace(Device.DeviceId) || Device.DeviceId.Length > 512)
        {
            Device.DeviceId = null;
        }

        Device.Name = Device.Name is { Length: > 0 } name ? Text.TextSanitizer.Sanitize(name, DeviceConfig.MaxNameLength) : null;

        if (!AllowedStartupDelays.Contains(Startup.DelaySeconds))
        {
            Startup.DelaySeconds = 0;
        }

        Appearance.Language = Enum.IsDefined(Appearance.Language) ? Appearance.Language : AppLanguage.System;
        Appearance.Theme = Enum.IsDefined(Appearance.Theme) ? Appearance.Theme : AppTheme.System;
        Startup.CloseAction = Enum.IsDefined(Startup.CloseAction) ? Startup.CloseAction : CloseAction.Tray;

        if (Updates.LastCheck is { } lastCheck && lastCheck > DateTimeOffset.UtcNow.AddDays(1))
        {
            Updates.LastCheck = null;
        }

        Notifications.MutedApps = [.. (Notifications.MutedApps ?? [])
            .Where(app => !string.IsNullOrWhiteSpace(app) && app.Length <= AncsLimits.MaxAppIdentifierLength)
            .Distinct(StringComparer.Ordinal)
            .Take(MaxMutedApps)];
        Notifications.MutedCategories = [.. (Notifications.MutedCategories ?? []).Where(Enum.IsDefined).Distinct()];

        AppNames = AppNames
            .Where(entry => entry.Key.Length is > 0 and <= AncsLimits.MaxAppIdentifierLength
                && entry.Value is { Length: > 0 and <= AncsLimits.MaxDisplayNameLength })
            .Take(MaxStoredAppNames)
            .ToDictionary(entry => entry.Key, entry => entry.Value, StringComparer.Ordinal);

        Version = CurrentVersion;
        return this;
    }
}

public sealed class DeviceConfig
{
    public const int MaxNameLength = 64;

    public string? DeviceId { get; set; }

    public string? Name { get; set; }

    public bool TryUpdateName(string? reportedName)
    {
        var clean = Text.TextSanitizer.Sanitize(reportedName ?? string.Empty, MaxNameLength);

        if (clean.Length == 0 || string.Equals(clean, Name, StringComparison.Ordinal))
        {
            return false;
        }

        Name = clean;
        return true;
    }
}

public sealed class StartupConfig
{
    public bool StartMinimized { get; set; } = true;

    public CloseAction CloseAction { get; set; } = CloseAction.Tray;

    public int DelaySeconds { get; set; }
}

public sealed class NotificationConfig
{
    public bool ShowPreviews { get; set; }

    public bool IgnoreSilent { get; set; } = true;

    public bool WindowsToasts { get; set; } = true;

    public List<string> MutedApps { get; set; } = [];

    public List<AncsCategory> MutedCategories { get; set; } = [];
}

public sealed class AppearanceConfig
{
    public AppLanguage Language { get; set; } = AppLanguage.System;

    public AppTheme Theme { get; set; } = AppTheme.System;
}

public sealed class UpdatesConfig
{
    public bool CheckAutomatically { get; set; } = true;

    public DateTimeOffset? LastCheck { get; set; }
}
