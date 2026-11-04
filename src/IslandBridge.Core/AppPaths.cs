namespace IslandBridge.Core;

public sealed record AppPaths(string DataDirectory)
{
    public static AppPaths Default { get; } = new(Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData, Environment.SpecialFolderOption.DoNotVerify),
        "IslandBridge"));

    public string ConfigFile => Path.Combine(DataDirectory, "config.json");

    public string LogDirectory => Path.Combine(DataDirectory, "logs");

    public string IconDirectory => Path.Combine(DataDirectory, "icons");
}
