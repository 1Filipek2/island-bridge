using System.Globalization;

namespace IslandBridge.AncsSpike;

internal sealed record SpikeOptions(string? DeviceName, bool ShowText, bool FetchExisting, TimeSpan? Duration)
{
    public static SpikeOptions Parse(ReadOnlySpan<string> args)
    {
        string? deviceName = null;
        var showText = false;
        var fetchExisting = false;
        TimeSpan? duration = null;

        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--device" when i + 1 < args.Length:
                    deviceName = args[++i];
                    break;
                case "--show-text":
                    showText = true;
                    break;
                case "--fetch-existing":
                    fetchExisting = true;
                    break;
                case "--seconds" when i + 1 < args.Length
                    && int.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out var seconds)
                    && seconds > 0:
                    duration = TimeSpan.FromSeconds(seconds);
                    i++;
                    break;
            }
        }

        return new SpikeOptions(deviceName, showText, fetchExisting, duration);
    }
}
