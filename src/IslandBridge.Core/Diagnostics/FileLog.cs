using System.Globalization;
using System.Text;

namespace IslandBridge.Core.Diagnostics;

public enum LogLevel
{
    Debug,
    Info,
    Warn,
    Error,
}

public interface ILog
{
    void Write(LogLevel level, string message);
}

public static class LogExtensions
{
    public static void Debug(this ILog log, string message) => log.Write(LogLevel.Debug, message);

    public static void Info(this ILog log, string message) => log.Write(LogLevel.Info, message);

    public static void Warn(this ILog log, string message) => log.Write(LogLevel.Warn, message);

    public static void Error(this ILog log, string message) => log.Write(LogLevel.Error, message);
}

public sealed class NullLog : ILog
{
    public static NullLog Instance { get; } = new();

    public void Write(LogLevel level, string message)
    {
    }
}

public sealed class FileLog : ILog, IDisposable
{
    public const long DefaultMaxFileBytes = 1024 * 1024;
    public const int DefaultRetainedFiles = 3;
    private const int MaxMessageLength = 512;

    private readonly Lock _gate = new();
    private readonly string _directory;
    private readonly string _baseName;
    private readonly long _maxFileBytes;
    private readonly int _retainedFiles;
    private readonly TimeProvider _timeProvider;
    private FileStream? _stream;

    public FileLog(
        string directory,
        string baseName = "islandbridge",
        long maxFileBytes = DefaultMaxFileBytes,
        int retainedFiles = DefaultRetainedFiles,
        LogLevel minimumLevel = LogLevel.Info,
        TimeProvider? timeProvider = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(directory);
        ArgumentException.ThrowIfNullOrWhiteSpace(baseName);
        ArgumentOutOfRangeException.ThrowIfLessThan(maxFileBytes, 1024);
        ArgumentOutOfRangeException.ThrowIfLessThan(retainedFiles, 1);

        _directory = directory;
        _baseName = baseName;
        _maxFileBytes = maxFileBytes;
        _retainedFiles = retainedFiles;
        _timeProvider = timeProvider ?? TimeProvider.System;
        MinimumLevel = minimumLevel;
    }

    public LogLevel MinimumLevel { get; set; }

    public string CurrentPath => System.IO.Path.Combine(_directory, _baseName + ".log");

    public void Write(LogLevel level, string message)
    {
        if (level < MinimumLevel)
        {
            return;
        }

        var safe = Text.TextSanitizer.Sanitize(message ?? string.Empty, MaxMessageLength);
        var line = string.Create(
            CultureInfo.InvariantCulture,
            $"{_timeProvider.GetLocalNow():yyyy-MM-dd HH:mm:ss.fff zzz} {LevelName(level)} {safe}{Environment.NewLine}");
        var bytes = Encoding.UTF8.GetBytes(line);

        lock (_gate)
        {
            try
            {
                var stream = EnsureStream();

                if (stream.Length + bytes.Length > _maxFileBytes)
                {
                    Rotate();
                    stream = EnsureStream();
                }

                stream.Write(bytes);
                stream.Flush();
            }
            catch (IOException)
            {
                CloseStream();
            }
            catch (UnauthorizedAccessException)
            {
                CloseStream();
            }
        }
    }

    public void Dispose()
    {
        lock (_gate)
        {
            CloseStream();
        }
    }

    private static string LevelName(LogLevel level) => level switch
    {
        LogLevel.Debug => "DBG",
        LogLevel.Info => "INF",
        LogLevel.Warn => "WRN",
        _ => "ERR",
    };

    private FileStream EnsureStream()
    {
        if (_stream is not null)
        {
            return _stream;
        }

        Directory.CreateDirectory(_directory);
        _stream = new FileStream(CurrentPath, FileMode.Append, FileAccess.Write, FileShare.Read | FileShare.Delete);
        return _stream;
    }

    private void Rotate()
    {
        CloseStream();
        var oldest = RotatedPath(_retainedFiles);

        if (File.Exists(oldest))
        {
            File.Delete(oldest);
        }

        for (var index = _retainedFiles - 1; index >= 1; index--)
        {
            var source = RotatedPath(index);

            if (File.Exists(source))
            {
                File.Move(source, RotatedPath(index + 1), overwrite: true);
            }
        }

        if (File.Exists(CurrentPath))
        {
            File.Move(CurrentPath, RotatedPath(1), overwrite: true);
        }
    }

    private string RotatedPath(int index) =>
        System.IO.Path.Combine(_directory, string.Create(CultureInfo.InvariantCulture, $"{_baseName}.{index}.log"));

    private void CloseStream()
    {
        _stream?.Dispose();
        _stream = null;
    }
}
