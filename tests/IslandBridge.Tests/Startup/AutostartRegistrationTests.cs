using IslandBridge.Core.Startup;
using Microsoft.Win32;

namespace IslandBridge.Tests.Startup;

public sealed class AutostartRegistrationTests : IDisposable
{
    private const string ExecutablePath = @"C:\Users\Test\AppData\Local\Programs\IslandBridge\IslandBridge.exe";

    private readonly string _testRoot = @"Software\IslandBridge.Tests\" + Guid.NewGuid().ToString("N");

    private string RunKey => _testRoot + @"\Run";

    private string ApprovedKey => _testRoot + @"\StartupApproved\Run";

    [Fact]
    public void Starts_disabled()
    {
        Assert.Equal(AutostartState.Disabled, Create().GetState());
    }

    [Fact]
    public void Enable_writes_a_quoted_command_with_the_autostart_switch()
    {
        var registration = Create();

        registration.Enable(ExecutablePath);

        Assert.Equal(AutostartState.Enabled, registration.GetState());
        Assert.Equal($"\"{ExecutablePath}\" --autostart", registration.GetCommand());
    }

    [Fact]
    public void Disable_removes_the_value()
    {
        var registration = Create();
        registration.Enable(ExecutablePath);

        registration.Disable();

        Assert.Equal(AutostartState.Disabled, registration.GetState());
    }

    [Theory]
    [InlineData(new byte[] { 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, AutostartState.DisabledByUser)]
    [InlineData(new byte[] { 0x07, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, AutostartState.DisabledByUser)]
    [InlineData(new byte[] { 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, AutostartState.Enabled)]
    [InlineData(new byte[] { 0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, AutostartState.Enabled)]
    public void Respects_task_manager_startup_approval(byte[] flags, AutostartState expected)
    {
        var registration = Create();
        registration.Enable(ExecutablePath);
        SetApproval(flags);

        Assert.Equal(expected, registration.GetState());
    }

    [Fact]
    public void Explicit_enable_clears_a_task_manager_disable()
    {
        var registration = Create();
        registration.Enable(ExecutablePath);
        SetApproval([0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);

        registration.Enable(ExecutablePath);

        Assert.Equal(AutostartState.Enabled, registration.GetState());
    }

    [Fact]
    public void Refresh_updates_a_moved_executable_but_never_enables()
    {
        var registration = Create();

        Assert.False(registration.RefreshPath(ExecutablePath));
        Assert.Equal(AutostartState.Disabled, registration.GetState());

        registration.Enable(@"C:\Old\IslandBridge.exe");

        Assert.True(registration.RefreshPath(ExecutablePath));
        Assert.Equal($"\"{ExecutablePath}\" --autostart", registration.GetCommand());
        Assert.False(registration.RefreshPath(ExecutablePath));
    }

    [Fact]
    public void Refresh_keeps_a_task_manager_disable()
    {
        var registration = Create();
        registration.Enable(@"C:\Old\IslandBridge.exe");
        SetApproval([0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);

        registration.RefreshPath(ExecutablePath);

        Assert.Equal(AutostartState.DisabledByUser, registration.GetState());
    }

    [Theory]
    [InlineData("IslandBridge.exe")]
    [InlineData("C:\\a\"b\\IslandBridge.exe")]
    public void Rejects_unsafe_paths(string path)
    {
        Assert.Throws<ArgumentException>(() => AutostartRegistration.BuildCommand(path));
    }

    public void Dispose() => Registry.CurrentUser.DeleteSubKeyTree(_testRoot, throwOnMissingSubKey: false);

    private AutostartRegistration Create() => new(Registry.CurrentUser, RunKey, ApprovedKey, "IslandBridge");

    private void SetApproval(byte[] flags)
    {
        using var key = Registry.CurrentUser.CreateSubKey(ApprovedKey, writable: true);
        key.SetValue("IslandBridge", flags, RegistryValueKind.Binary);
    }
}
