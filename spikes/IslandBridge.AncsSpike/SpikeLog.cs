using System.Diagnostics;
using System.Globalization;

namespace IslandBridge.AncsSpike;

internal static class SpikeLog
{
    private static readonly long Start = Stopwatch.GetTimestamp();
    private static readonly Lock Gate = new();

    public static void Write(string message)
    {
        var elapsed = Stopwatch.GetElapsedTime(Start);
        var line = string.Create(CultureInfo.InvariantCulture, $"{DateTime.Now:HH:mm:ss.fff} +{elapsed.TotalMilliseconds,9:F1} ms  {message}");

        lock (Gate)
        {
            Console.WriteLine(line);
        }
    }
}
