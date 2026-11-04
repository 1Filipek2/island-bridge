using Microsoft.Win32;

namespace IslandBridge.Core.Startup;

public enum AutostartState
{
    Disabled,
    Enabled,
    DisabledByUser,
}

public sealed class AutostartRegistration
{
    public const string DefaultRunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    public const string DefaultApprovedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";
    public const string DefaultValueName = "IslandBridge";

    private readonly RegistryKey _root;
    private readonly string _runKey;
    private readonly string _approvedKey;
    private readonly string _valueName;

    public AutostartRegistration(
        RegistryKey? root = null,
        string runKey = DefaultRunKey,
        string approvedKey = DefaultApprovedKey,
        string valueName = DefaultValueName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(runKey);
        ArgumentException.ThrowIfNullOrWhiteSpace(approvedKey);
        ArgumentException.ThrowIfNullOrWhiteSpace(valueName);

        _root = root ?? Registry.CurrentUser;
        _runKey = runKey;
        _approvedKey = approvedKey;
        _valueName = valueName;
    }

    public static string BuildCommand(string executablePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(executablePath);

        if (executablePath.Contains('"', StringComparison.Ordinal) || !Path.IsPathFullyQualified(executablePath))
        {
            throw new ArgumentException("The executable path must be fully qualified and must not contain quotes.", nameof(executablePath));
        }

        return $"\"{executablePath}\" {StartupArguments.AutostartSwitch}";
    }

    public AutostartState GetState()
    {
        using var run = _root.OpenSubKey(_runKey);

        if (run?.GetValue(_valueName) is not string)
        {
            return AutostartState.Disabled;
        }

        using var approved = _root.OpenSubKey(_approvedKey);
        return approved?.GetValue(_valueName) is byte[] { Length: > 0 } flags && (flags[0] & 1) == 1
            ? AutostartState.DisabledByUser
            : AutostartState.Enabled;
    }

    public string? GetCommand()
    {
        using var run = _root.OpenSubKey(_runKey);
        return run?.GetValue(_valueName) as string;
    }

    public void Enable(string executablePath)
    {
        var command = BuildCommand(executablePath);

        using (var run = _root.CreateSubKey(_runKey, writable: true))
        {
            run.SetValue(_valueName, command, RegistryValueKind.String);
        }

        using var approved = _root.OpenSubKey(_approvedKey, writable: true);

        if (approved?.GetValue(_valueName) is byte[] { Length: > 0 } flags && (flags[0] & 1) == 1)
        {
            approved.DeleteValue(_valueName, throwOnMissingValue: false);
        }
    }

    public void Disable()
    {
        using var run = _root.OpenSubKey(_runKey, writable: true);
        run?.DeleteValue(_valueName, throwOnMissingValue: false);
    }

    public bool RefreshPath(string executablePath)
    {
        var command = BuildCommand(executablePath);
        var current = GetCommand();

        if (current is null || string.Equals(current, command, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        using var run = _root.CreateSubKey(_runKey, writable: true);
        run.SetValue(_valueName, command, RegistryValueKind.String);
        return true;
    }
}
