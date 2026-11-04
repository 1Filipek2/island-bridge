using System.Diagnostics;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Notifications;

namespace IslandBridge.Core.Icons;

public sealed record IconRequest(NotificationOrigin Origin, string AppIdentifier, string AppName);

public interface IAppIconSource
{
    byte[]? TryGetPng(IconRequest request, CancellationToken cancellationToken);
}

public sealed class IconResolver
{
    private readonly IconCache _cache;
    private readonly IAppIconSource _source;
    private readonly ILog _log;

    public IconResolver(IconCache cache, IAppIconSource source, ILog log)
    {
        ArgumentNullException.ThrowIfNull(cache);
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(log);

        _cache = cache;
        _source = source;
        _log = log;
    }

    public static string? KeyFor(IslandNotification notification)
    {
        ArgumentNullException.ThrowIfNull(notification);

        var identity = notification.AppIdentifier.Length > 0
            ? notification.AppIdentifier.ToLowerInvariant()
            : AppNameMatcher.Normalize(notification.AppName);

        if (identity.Length == 0)
        {
            return null;
        }

        return notification.Origin switch
        {
            NotificationOrigin.Windows => "windows:" + identity,
            _ => "iphone:" + identity,
        };
    }

    public byte[]? Resolve(IslandNotification notification, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(notification);

        if (KeyFor(notification) is not { } key)
        {
            return null;
        }

        var cached = _cache.Lookup(key);

        if (cached.Result != IconLookupResult.Unknown)
        {
            return cached.Png;
        }

        var started = Stopwatch.GetTimestamp();
        byte[]? png;

        try
        {
            png = _source.TryGetPng(new IconRequest(notification.Origin, notification.AppIdentifier, notification.AppName), cancellationToken);
        }
        catch (OperationCanceledException)
        {
            return null;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _log.Warn($"icon lookup failed: {exception.GetType().Name} 0x{exception.HResult:X8}");
            png = null;
        }

        if (png is null)
        {
            _cache.StoreMiss(key);
        }
        else
        {
            _cache.Store(key, png);
        }

        _log.Info($"icon app={notification.AppIdentifier} {(png is null ? "none" : $"{png.Length} bytes")} in {Stopwatch.GetElapsedTime(started).TotalMilliseconds:F0} ms");
        return png is null ? null : _cache.Lookup(key).Png;
    }
}
