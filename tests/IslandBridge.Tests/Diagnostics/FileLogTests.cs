using IslandBridge.Core.Diagnostics;

namespace IslandBridge.Tests.Diagnostics;

public sealed class FileLogTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "IslandBridge.Tests", Guid.NewGuid().ToString("N"));

    [Fact]
    public void Writes_lines_at_or_above_the_minimum_level()
    {
        using (var log = new FileLog(_directory, minimumLevel: LogLevel.Info))
        {
            log.Debug("hidden");
            log.Info("shown");
            log.Error("also shown");
        }

        var text = File.ReadAllText(Path.Combine(_directory, "islandbridge.log"));

        Assert.DoesNotContain("hidden", text, StringComparison.Ordinal);
        Assert.Contains("INF shown", text, StringComparison.Ordinal);
        Assert.Contains("ERR also shown", text, StringComparison.Ordinal);
    }

    [Fact]
    public void Sanitizes_messages_so_one_entry_is_one_line()
    {
        using (var log = new FileLog(_directory))
        {
            log.Info("first\nsecond\u202E");
        }

        var lines = File.ReadAllLines(Path.Combine(_directory, "islandbridge.log"));

        Assert.Single(lines);
        Assert.EndsWith("INF first second", lines[0], StringComparison.Ordinal);
    }

    [Fact]
    public void Rotates_and_keeps_a_bounded_number_of_files()
    {
        using (var log = new FileLog(_directory, maxFileBytes: 2048, retainedFiles: 2))
        {
            for (var i = 0; i < 400; i++)
            {
                log.Info($"entry {i:D4} with some padding to fill the file");
            }
        }

        var files = Directory.GetFiles(_directory).Select(Path.GetFileName).Order(StringComparer.Ordinal).ToArray();

        Assert.Equal(["islandbridge.1.log", "islandbridge.2.log", "islandbridge.log"], files);
        Assert.All(Directory.GetFiles(_directory), file => Assert.True(new FileInfo(file).Length <= 2048));
        Assert.Contains("entry 0399", File.ReadAllText(Path.Combine(_directory, "islandbridge.log")), StringComparison.Ordinal);
    }

    public void Dispose()
    {
        if (Directory.Exists(_directory))
        {
            Directory.Delete(_directory, recursive: true);
        }
    }
}
