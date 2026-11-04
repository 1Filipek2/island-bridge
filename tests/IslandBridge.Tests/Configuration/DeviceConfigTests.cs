using IslandBridge.Core.Configuration;

namespace IslandBridge.Tests.Configuration;

public sealed class DeviceConfigTests
{
    [Fact]
    public void A_new_reported_name_replaces_the_stored_one()
    {
        var device = new DeviceConfig { Name = "4" };

        Assert.True(device.TryUpdateName("Findo"));
        Assert.Equal("Findo", device.Name);
    }

    [Fact]
    public void The_same_name_is_not_an_update()
    {
        var device = new DeviceConfig { Name = "Phone" };

        Assert.False(device.TryUpdateName("Phone"));
        Assert.False(device.TryUpdateName("  Phone  "));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("\u202E\u0007")]
    public void Empty_or_unusable_names_keep_the_stored_one(string? reported)
    {
        var device = new DeviceConfig { Name = "Phone" };

        Assert.False(device.TryUpdateName(reported));
        Assert.Equal("Phone", device.Name);
    }

    [Fact]
    public void Reported_names_are_sanitized_and_capped()
    {
        var device = new DeviceConfig();

        Assert.True(device.TryUpdateName("A\r\nB" + new string('x', 100)));
        Assert.StartsWith("A B", device.Name, StringComparison.Ordinal);
        Assert.Equal(DeviceConfig.MaxNameLength, device.Name!.Length);
    }
}
