using IslandBridge.Core.Configuration;

namespace IslandBridge.Core.Startup;

public sealed record StartupArguments(bool Autostart, bool Minimized, bool Show)
{
    public const string AutostartSwitch = "--autostart";
    public const string MinimizedSwitch = "--minimized";
    public const string ShowSwitch = "--show";

    public static StartupArguments Parse(IEnumerable<string> args)
    {
        ArgumentNullException.ThrowIfNull(args);

        var autostart = false;
        var minimized = false;
        var show = false;

        foreach (var arg in args)
        {
            switch (arg)
            {
                case AutostartSwitch:
                    autostart = true;
                    break;
                case MinimizedSwitch:
                    minimized = true;
                    break;
                case ShowSwitch:
                    show = true;
                    break;
            }
        }

        return new StartupArguments(autostart, minimized, show);
    }

    public bool ShouldShowWindow(StartupConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);

        if (Show)
        {
            return true;
        }

        if (Minimized)
        {
            return false;
        }

        return !Autostart || !config.StartMinimized;
    }

    public TimeSpan BluetoothDelay(StartupConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);
        return Autostart ? TimeSpan.FromSeconds(config.DelaySeconds) : TimeSpan.Zero;
    }
}
