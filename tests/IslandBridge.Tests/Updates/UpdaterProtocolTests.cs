using IslandBridge.Core.Configuration;
using IslandBridge.Core.Updates;

namespace IslandBridge.Tests.Updates;

public sealed class UpdaterProtocolTests
{
    [Theory]
    [InlineData("1.0.0", 1, 0, 0)]
    [InlineData("0.0.1", 0, 0, 1)]
    [InlineData("10.20.9999", 10, 20, 9999)]
    public void Parses_versions(string text, int major, int minor, int patch)
    {
        Assert.True(UpdateVersion.TryParse(text, out var version));
        Assert.Equal(new UpdateVersion(major, minor, patch), version);
        Assert.Equal(text, version.ToString());
    }

    [Theory]
    [InlineData("")]
    [InlineData("1.0")]
    [InlineData("1.0.0.0")]
    [InlineData("01.0.0")]
    [InlineData("1.0.0-beta")]
    [InlineData("1..0")]
    [InlineData("+1.0.0")]
    [InlineData("1.0.10000")]
    [InlineData(" 1.0.0")]
    [InlineData("\u0661.0.0")]
    public void Rejects_bad_versions(string text)
    {
        Assert.False(UpdateVersion.TryParse(text, out _));
    }

    [Fact]
    public void Compares_versions_numerically()
    {
        Assert.True(new UpdateVersion(1, 10, 0) > new UpdateVersion(1, 9, 9));
        Assert.True(new UpdateVersion(2, 0, 0) > new UpdateVersion(1, 99, 99));
        Assert.True(new UpdateVersion(1, 0, 1) > new UpdateVersion(1, 0, 0));
        Assert.False(new UpdateVersion(1, 0, 0) > new UpdateVersion(1, 0, 0));
    }

    [Fact]
    public void The_build_knows_its_own_version()
    {
        Assert.NotEqual(default, UpdateVersion.Current);
    }

    [Fact]
    public void Parses_updater_commands_strictly()
    {
        Assert.Equal(new UpdaterCommand(UpdaterVerb.Check, 0), UpdaterCommand.Parse(["check"]));
        Assert.Equal(new UpdaterCommand(UpdaterVerb.Install, 1234), UpdaterCommand.Parse(["install", "--wait-pid", "1234"]));

        string[][] invalid =
        [
            [],
            ["CHECK"],
            ["check", "extra"],
            ["install"],
            ["install", "--wait-pid"],
            ["install", "--wait-pid", "0"],
            ["install", "--wait-pid", "-1"],
            ["install", "--wait-pid", "99999999999"],
            ["install", "--wait-pid", "12a"],
            ["install", "--url", "https://evil.example"],
        ];

        foreach (var args in invalid)
        {
            Assert.Equal(UpdaterVerb.Invalid, UpdaterCommand.Parse(args).Verb);
        }
    }

    [Fact]
    public void Reports_round_trip()
    {
        UpdaterReport[] reports =
        [
            UpdaterReport.Current(new UpdateVersion(1, 0, 0)),
            UpdaterReport.Available(new UpdateVersion(1, 1, 0)),
            UpdaterReport.Ready(new UpdateVersion(1, 1, 0)),
            .. Enum.GetValues<UpdateFailure>().Where(failure => failure != UpdateFailure.None).Select(UpdaterReport.Failed),
        ];

        foreach (var report in reports)
        {
            Assert.True(UpdaterReport.TryParse(report.ToString(), out var parsed));
            Assert.Equal(report, parsed);
        }
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("available")]
    [InlineData("available 1.1")]
    [InlineData("Available 1.1.0")]
    [InlineData("available  1.1.0")]
    [InlineData("ready 1.1.0 extra")]
    [InlineData("failed none")]
    [InlineData("failed whatever")]
    [InlineData("installed 1.1.0")]
    public void Rejects_bad_report_lines(string? line)
    {
        Assert.False(UpdaterReport.TryParse(line, out _));
    }

    [Fact]
    public void Rejects_long_report_lines()
    {
        Assert.False(UpdaterReport.TryParse("available 1.1.0" + new string(' ', UpdaterReport.MaxLineLength), out _));
    }

    [Fact]
    public void Schedules_the_next_check()
    {
        var now = new DateTimeOffset(2026, 11, 9, 12, 0, 0, TimeSpan.Zero);

        Assert.Equal(UpdateSchedule.MinimumDelay, UpdateSchedule.NextCheckDelay(now, null));
        Assert.Equal(UpdateSchedule.MinimumDelay, UpdateSchedule.NextCheckDelay(now, now.AddDays(-3)));
        Assert.Equal(UpdateSchedule.MinimumDelay, UpdateSchedule.NextCheckDelay(now, now.AddHours(5)));
        Assert.Equal(TimeSpan.FromHours(20), UpdateSchedule.NextCheckDelay(now, now.AddHours(-4)));
    }

    [Fact]
    public void Compares_install_directories()
    {
        Assert.True(InstallLocation.SameDirectory(@"C:\Users\x\AppData\Local\Programs\IslandBridge\", @"c:\users\x\appdata\local\programs\islandbridge"));
        Assert.False(InstallLocation.SameDirectory(@"C:\Programs\IslandBridge", @"C:\Programs\IslandBridge2"));
        Assert.False(InstallLocation.SameDirectory(string.Empty, @"C:\Programs\IslandBridge"));
    }

    [Fact]
    public void Config_drops_a_last_check_from_the_future()
    {
        var config = new AppConfig { Updates = new UpdatesConfig { LastCheck = DateTimeOffset.UtcNow.AddDays(30) } }.Normalize();

        Assert.Null(config.Updates.LastCheck);
        Assert.True(config.Updates.CheckAutomatically);
    }
}
