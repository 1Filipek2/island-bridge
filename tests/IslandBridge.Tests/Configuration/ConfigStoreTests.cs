using IslandBridge.Core.Ancs;
using IslandBridge.Core.Configuration;
using IslandBridge.Core.Diagnostics;

namespace IslandBridge.Tests.Configuration;

public sealed class ConfigStoreTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "IslandBridge.Tests", Guid.NewGuid().ToString("N"));

    private string ConfigPath => Path.Combine(_directory, "config.json");

    [Fact]
    public void Missing_file_gives_defaults()
    {
        var store = new ConfigStore(ConfigPath, NullLog.Instance);

        var config = store.Load();

        Assert.Equal(ConfigLoadOutcome.Missing, store.LastOutcome);
        Assert.True(config.Startup.StartMinimized);
        Assert.Equal(CloseAction.Tray, config.Startup.CloseAction);
        Assert.True(config.Notifications.IgnoreSilent);
        Assert.False(config.Notifications.ShowPreviews);
        Assert.True(config.Notifications.WindowsToasts);
    }

    [Fact]
    public void Round_trips_every_section()
    {
        var store = new ConfigStore(ConfigPath, NullLog.Instance);
        var config = new AppConfig
        {
            FirstRunCompleted = true,
            Device = { DeviceId = "BluetoothLE#BluetoothLE00:00:00:00:00:00-11:22:33:44:55:66", Name = "Phone" },
            Startup = { StartMinimized = false, DelaySeconds = 30, CloseAction = CloseAction.Exit },
            Notifications = { ShowPreviews = true, IgnoreSilent = false, WindowsToasts = false, MutedApps = ["com.example"], MutedCategories = [AncsCategory.News] },
            Appearance = { Language = AppLanguage.Slovak, Theme = AppTheme.Dark },
            AppNames = { ["net.whatsapp.WhatsApp"] = "WhatsApp" },
        };

        store.Save(config);
        var loaded = store.Load();

        Assert.Equal(ConfigLoadOutcome.Loaded, store.LastOutcome);
        Assert.True(loaded.FirstRunCompleted);
        Assert.Equal(config.Device.DeviceId, loaded.Device.DeviceId);
        Assert.Equal("Phone", loaded.Device.Name);
        Assert.False(loaded.Startup.StartMinimized);
        Assert.Equal(30, loaded.Startup.DelaySeconds);
        Assert.Equal(CloseAction.Exit, loaded.Startup.CloseAction);
        Assert.True(loaded.Notifications.ShowPreviews);
        Assert.False(loaded.Notifications.IgnoreSilent);
        Assert.False(loaded.Notifications.WindowsToasts);
        Assert.Equal(["com.example"], loaded.Notifications.MutedApps);
        Assert.Equal([AncsCategory.News], loaded.Notifications.MutedCategories);
        Assert.Equal(AppLanguage.Slovak, loaded.Appearance.Language);
        Assert.Equal(AppTheme.Dark, loaded.Appearance.Theme);
        Assert.Equal("WhatsApp", loaded.AppNames["net.whatsapp.WhatsApp"]);
        Assert.False(File.Exists(ConfigPath + ".tmp"));
    }

    [Fact]
    public void Enums_are_written_as_strings()
    {
        var store = new ConfigStore(ConfigPath, NullLog.Instance);
        store.Save(new AppConfig { Appearance = { Theme = AppTheme.Dark } });

        Assert.Contains("\"theme\": \"Dark\"", File.ReadAllText(ConfigPath), StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("{ not json")]
    [InlineData("null")]
    [InlineData("[1, 2, 3]")]
    [InlineData("{\"startup\": {\"delaySeconds\": \"soon\"}}")]
    public void Corrupt_file_is_quarantined_and_defaults_are_used(string content)
    {
        Directory.CreateDirectory(_directory);
        File.WriteAllText(ConfigPath, content);
        var store = new ConfigStore(ConfigPath, NullLog.Instance);

        var config = store.Load();

        Assert.Equal(ConfigLoadOutcome.Corrupt, store.LastOutcome);
        Assert.True(config.Startup.StartMinimized);
        Assert.Equal(CloseAction.Tray, config.Startup.CloseAction);
        Assert.Equal(content, File.ReadAllText(Path.Combine(_directory, "config.bad.json")));
    }

    [Fact]
    public void Oversized_file_is_rejected()
    {
        Directory.CreateDirectory(_directory);
        File.WriteAllText(ConfigPath, new string(' ', (int)ConfigStore.MaxFileBytes + 1));
        var store = new ConfigStore(ConfigPath, NullLog.Instance);

        store.Load();

        Assert.Equal(ConfigLoadOutcome.TooLarge, store.LastOutcome);
    }

    [Fact]
    public void Out_of_range_values_are_normalized()
    {
        Directory.CreateDirectory(_directory);
        File.WriteAllText(ConfigPath, """
            {
              "version": 1,
              "device": { "deviceId": "   ", "name": "\u202Eevil" },
              "startup": { "delaySeconds": 17 },
              "notifications": { "mutedApps": ["a", "a", ""], "mutedCategories": ["News", "News"] },
              "appNames": { "": "x", "ok": "" , "good": "Good" },
            }
            """);
        var store = new ConfigStore(ConfigPath, NullLog.Instance);

        var config = store.Load();

        Assert.Null(config.Device.DeviceId);
        Assert.Equal("evil", config.Device.Name);
        Assert.Equal(0, config.Startup.DelaySeconds);
        Assert.Equal(["a"], config.Notifications.MutedApps);
        Assert.Equal([AncsCategory.News], config.Notifications.MutedCategories);
        Assert.True(config.Notifications.WindowsToasts);
        Assert.Equal(["good"], config.AppNames.Keys);
    }

    [Fact]
    public void Newer_version_is_backed_up_before_use()
    {
        Directory.CreateDirectory(_directory);
        File.WriteAllText(ConfigPath, "{\"version\": 9}");
        var store = new ConfigStore(ConfigPath, NullLog.Instance);

        var config = store.Load();

        Assert.Equal(AppConfig.CurrentVersion, config.Version);
        Assert.True(File.Exists(Path.Combine(_directory, "config.v9.json")));
    }

    public void Dispose()
    {
        if (Directory.Exists(_directory))
        {
            Directory.Delete(_directory, recursive: true);
        }
    }
}
