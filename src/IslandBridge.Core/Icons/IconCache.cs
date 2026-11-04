using System.Security.Cryptography;
using System.Text;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Island;

namespace IslandBridge.Core.Icons;

public sealed class IconCache
{
    public const int DefaultMaxFiles = 256;
    public const int DefaultMemoryEntries = 32;

    private readonly string _directory;
    private readonly TimeProvider _timeProvider;
    private readonly ILog _log;
    private readonly int _maxFiles;
    private readonly int _memoryEntries;
    private readonly TimeSpan _missLifetime;
    private readonly Lock _gate = new();
    private readonly Dictionary<string, LinkedListNode<(string Key, byte[] Png)>> _memory = new(StringComparer.Ordinal);
    private readonly LinkedList<(string Key, byte[] Png)> _recency = new();
    private readonly Dictionary<string, DateTimeOffset> _misses = new(StringComparer.Ordinal);

    public IconCache(
        string directory,
        TimeProvider timeProvider,
        ILog log,
        int maxFiles = DefaultMaxFiles,
        int memoryEntries = DefaultMemoryEntries,
        TimeSpan? missLifetime = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(directory);
        ArgumentNullException.ThrowIfNull(timeProvider);
        ArgumentNullException.ThrowIfNull(log);
        ArgumentOutOfRangeException.ThrowIfLessThan(maxFiles, 1);
        ArgumentOutOfRangeException.ThrowIfLessThan(memoryEntries, 1);

        _directory = directory;
        _timeProvider = timeProvider;
        _log = log;
        _maxFiles = maxFiles;
        _memoryEntries = memoryEntries;
        _missLifetime = missLifetime ?? TimeSpan.FromHours(1);
    }

    public int MemoryCount
    {
        get
        {
            lock (_gate)
            {
                return _memory.Count;
            }
        }
    }

    public IconLookup Lookup(string key)
    {
        ArgumentException.ThrowIfNullOrEmpty(key);

        lock (_gate)
        {
            if (_memory.TryGetValue(key, out var node))
            {
                _recency.Remove(node);
                _recency.AddFirst(node);
                return new IconLookup(IconLookupResult.Found, node.Value.Png);
            }

            if (_misses.TryGetValue(key, out var until))
            {
                if (_timeProvider.GetUtcNow() < until)
                {
                    return new IconLookup(IconLookupResult.KnownMissing, null);
                }

                _misses.Remove(key);
            }
        }

        var png = ReadFile(key);

        if (png is null)
        {
            return new IconLookup(IconLookupResult.Unknown, null);
        }

        Remember(key, png);
        return new IconLookup(IconLookupResult.Found, png);
    }

    public void Store(string key, byte[] png)
    {
        ArgumentException.ThrowIfNullOrEmpty(key);
        ArgumentNullException.ThrowIfNull(png);

        if (!IsAcceptable(png))
        {
            StoreMiss(key);
            return;
        }

        Remember(key, png);
        WriteFile(key, png);
    }

    public void StoreMiss(string key)
    {
        ArgumentException.ThrowIfNullOrEmpty(key);

        lock (_gate)
        {
            if (_misses.Count >= _maxFiles)
            {
                _misses.Clear();
            }

            _misses[key] = _timeProvider.GetUtcNow() + _missLifetime;
        }
    }

    internal static string FileName(string key)
    {
        Span<byte> hash = stackalloc byte[SHA256.HashSizeInBytes];
        SHA256.HashData(Encoding.UTF8.GetBytes(key), hash);
        return Convert.ToHexStringLower(hash[..16]) + ".png";
    }

    private static bool IsAcceptable(ReadOnlySpan<byte> png) =>
        png.Length is > 8 and <= IslandProtocol.MaxIconBytes && PngWriter.HasSignature(png);

    private void Remember(string key, byte[] png)
    {
        lock (_gate)
        {
            _misses.Remove(key);

            if (_memory.TryGetValue(key, out var existing))
            {
                _recency.Remove(existing);
                _memory.Remove(key);
            }

            _memory[key] = _recency.AddFirst((key, png));

            while (_memory.Count > _memoryEntries && _recency.Last is { } oldest)
            {
                _recency.RemoveLast();
                _memory.Remove(oldest.Value.Key);
            }
        }
    }

    private byte[]? ReadFile(string key)
    {
        var path = Path.Combine(_directory, FileName(key));

        try
        {
            var info = new FileInfo(path);

            if (!info.Exists || info.Length > IslandProtocol.MaxIconBytes)
            {
                return null;
            }

            var png = File.ReadAllBytes(path);
            return IsAcceptable(png) ? png : null;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            _log.Warn($"icon cache read failed: {exception.GetType().Name}");
            return null;
        }
    }

    private void WriteFile(string key, byte[] png)
    {
        try
        {
            Directory.CreateDirectory(_directory);
            var path = Path.Combine(_directory, FileName(key));
            var temporary = path + ".tmp";
            File.WriteAllBytes(temporary, png);
            File.Move(temporary, path, overwrite: true);
            Trim(path);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            _log.Warn($"icon cache write failed: {exception.GetType().Name}");
        }
    }

    private void Trim(string keep)
    {
        var files = new DirectoryInfo(_directory).GetFiles("*.png");

        if (files.Length <= _maxFiles)
        {
            return;
        }

        foreach (var file in files
            .Where(file => !string.Equals(file.FullName, Path.GetFullPath(keep), StringComparison.OrdinalIgnoreCase))
            .OrderBy(file => file.LastWriteTimeUtc)
            .Take(files.Length - _maxFiles))
        {
            file.Delete();
        }
    }
}

public enum IconLookupResult
{
    Unknown,
    Found,
    KnownMissing,
}

public readonly record struct IconLookup(IconLookupResult Result, byte[]? Png);
