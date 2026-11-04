using IslandBridge.Core.Configuration;
using IslandBridge.Core.Startup;

namespace IslandBridge.Tests.Startup;

public sealed class StartupArgumentsTests
{
    [Fact]
    public void Parses_known_switches_and_ignores_the_rest()
    {
        var args = StartupArguments.Parse(["--autostart", "--unknown", "/show", "--MINIMIZED", "--show"]);

        Assert.Equal(new StartupArguments(Autostart: true, Minimized: false, Show: true), args);
    }

    [Theory]
    [InlineData(new string[0], true, true)]
    [InlineData(new[] { "--autostart" }, true, false)]
    [InlineData(new[] { "--autostart" }, false, true)]
    [InlineData(new[] { "--minimized" }, false, false)]
    [InlineData(new[] { "--autostart", "--show" }, true, true)]
    [InlineData(new[] { "--minimized", "--show" }, true, true)]
    public void Decides_whether_to_show_the_window(string[] raw, bool startMinimized, bool expected)
    {
        var args = StartupArguments.Parse(raw);

        Assert.Equal(expected, args.ShouldShowWindow(new StartupConfig { StartMinimized = startMinimized }));
    }

    [Fact]
    public void Bluetooth_delay_applies_only_to_autostart()
    {
        var config = new StartupConfig { DelaySeconds = 30 };

        Assert.Equal(TimeSpan.FromSeconds(30), StartupArguments.Parse(["--autostart"]).BluetoothDelay(config));
        Assert.Equal(TimeSpan.Zero, StartupArguments.Parse([]).BluetoothDelay(config));
    }
}
