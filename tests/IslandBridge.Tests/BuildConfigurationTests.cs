using System.Reflection;

namespace IslandBridge.Tests;

public sealed class BuildConfigurationTests
{
    [Fact]
    public void App_assembly_targets_windows()
    {
        var attribute = typeof(Program).Assembly.GetCustomAttribute<System.Runtime.Versioning.TargetPlatformAttribute>();

        Assert.NotNull(attribute);
        Assert.StartsWith("Windows10.0.22621", attribute.PlatformName, StringComparison.Ordinal);
    }
}
