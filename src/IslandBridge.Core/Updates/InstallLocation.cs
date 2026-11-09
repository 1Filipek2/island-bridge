using Microsoft.Win32;

namespace IslandBridge.Core.Updates;

// Only an installer-made copy updates itself; a portable folder gets a link to the release page.
public static class InstallLocation
{
    public const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\{6A4F1C2E-8B3D-4F7A-9E21-5C0D7B9A3F48}_is1";

    public static bool IsInstalledCopy(string directory)
    {
        ArgumentNullException.ThrowIfNull(directory);

        using var key = Registry.CurrentUser.OpenSubKey(UninstallKey);
        return key?.GetValue("InstallLocation") is string location
            && SameDirectory(location, directory)
            && File.Exists(Path.Combine(directory, "unins000.exe"));
    }

    public static bool SameDirectory(string left, string right)
    {
        ArgumentNullException.ThrowIfNull(left);
        ArgumentNullException.ThrowIfNull(right);

        if (left.Length == 0 || right.Length == 0)
        {
            return false;
        }

        return string.Equals(Normalize(left), Normalize(right), StringComparison.OrdinalIgnoreCase);
    }

    private static string Normalize(string path) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
}
