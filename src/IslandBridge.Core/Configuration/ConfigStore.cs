using System.Text.Json;
using System.Text.Json.Serialization;
using IslandBridge.Core.Diagnostics;

namespace IslandBridge.Core.Configuration;

[JsonSourceGenerationOptions(
    WriteIndented = true,
    UseStringEnumConverter = true,
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    ReadCommentHandling = JsonCommentHandling.Skip,
    AllowTrailingCommas = true,
    MaxDepth = 16)]
[JsonSerializable(typeof(AppConfig))]
internal sealed partial class ConfigJsonContext : JsonSerializerContext;

public enum ConfigLoadOutcome
{
    Loaded,
    Missing,
    Corrupt,
    TooLarge,
}

public sealed class ConfigStore
{
    public const long MaxFileBytes = 1024 * 1024;

    private readonly ILog _log;

    public ConfigStore(string path, ILog log)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(log);

        Path = path;
        _log = log;
    }

    public string Path { get; }

    public ConfigLoadOutcome LastOutcome { get; private set; }

    public AppConfig Load()
    {
        var file = new FileInfo(Path);

        if (!file.Exists)
        {
            LastOutcome = ConfigLoadOutcome.Missing;
            return new AppConfig();
        }

        if (file.Length > MaxFileBytes)
        {
            LastOutcome = ConfigLoadOutcome.TooLarge;
            Quarantine("too large");
            return new AppConfig();
        }

        try
        {
            using var stream = file.OpenRead();
            var config = JsonSerializer.Deserialize(stream, ConfigJsonContext.Default.AppConfig);

            if (config is null)
            {
                LastOutcome = ConfigLoadOutcome.Corrupt;
                Quarantine("empty");
                return new AppConfig();
            }

            if (config.Version > AppConfig.CurrentVersion)
            {
                _log.Warn($"config version {config.Version} is newer than {AppConfig.CurrentVersion}; keeping a copy");
                Copy($".v{config.Version}.json");
            }

            LastOutcome = ConfigLoadOutcome.Loaded;
            return config.Normalize();
        }
        catch (JsonException exception)
        {
            LastOutcome = ConfigLoadOutcome.Corrupt;
            Quarantine($"invalid JSON at line {exception.LineNumber}");
            return new AppConfig();
        }
        catch (IOException exception)
        {
            LastOutcome = ConfigLoadOutcome.Corrupt;
            _log.Error($"config read failed: {exception.GetType().Name}");
            return new AppConfig();
        }
    }

    public void Save(AppConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);

        var directory = System.IO.Path.GetDirectoryName(Path)!;
        Directory.CreateDirectory(directory);
        var temporary = Path + ".tmp";

        using (var stream = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            JsonSerializer.Serialize(stream, config.Normalize(), ConfigJsonContext.Default.AppConfig);
            stream.Flush(flushToDisk: true);
        }

        File.Move(temporary, Path, overwrite: true);
    }

    private void Quarantine(string reason)
    {
        _log.Warn($"config unusable ({reason}); starting with defaults");
        Copy(".bad.json");
    }

    private void Copy(string suffix)
    {
        try
        {
            File.Copy(Path, System.IO.Path.ChangeExtension(Path, null) + suffix, overwrite: true);
        }
        catch (IOException exception)
        {
            _log.Error($"config backup failed: {exception.GetType().Name}");
        }
        catch (UnauthorizedAccessException exception)
        {
            _log.Error($"config backup failed: {exception.GetType().Name}");
        }
    }
}
