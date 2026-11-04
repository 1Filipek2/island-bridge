using System.Collections.Concurrent;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Icons;
using IslandBridge.Core.Island;
using IslandBridge.Core.Notifications;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.Graphics.Gdi;
using Windows.Win32.UI.Shell;

namespace IslandBridge.Icons;

internal sealed unsafe class ShellAppIconSource : IAppIconSource, IDisposable
{
    private const string AppsFolder = "shell:AppsFolder";
    private const int MaxApps = 4096;
    private const int MaxParsingNameLength = 512;

    private static readonly TimeSpan IndexLifetime = TimeSpan.FromMinutes(10);
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(3);

    private readonly ILog _log;
    private readonly BlockingCollection<WorkItem> _work = new(boundedCapacity: 16);
    private readonly Thread _thread;
    private List<InstalledApp>? _apps;
    private long _indexedAt;

    public ShellAppIconSource(ILog log)
    {
        ArgumentNullException.ThrowIfNull(log);
        _log = log;
        _thread = new Thread(Run) { Name = "IslandBridge Icons", IsBackground = true };
        _thread.SetApartmentState(ApartmentState.STA);
        _thread.Start();
    }

    private static ReadOnlySpan<int> Sizes => [64, 48, 32];

    public byte[]? TryGetPng(IconRequest request, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);

        var item = new WorkItem(request, new TaskCompletionSource<byte[]?>(TaskCreationOptions.RunContinuationsAsynchronously));

        if (!_work.TryAdd(item, 0, cancellationToken))
        {
            return null;
        }

        try
        {
            return item.Completion.Task.Wait(RequestTimeout, cancellationToken) ? item.Completion.Task.Result : null;
        }
        catch (AggregateException)
        {
            return null;
        }
    }

    public void Dispose()
    {
        _work.CompleteAdding();
        _thread.Join(TimeSpan.FromSeconds(2));
        _work.Dispose();
    }

    private static string? ReadName(IShellItem* item, SIGDN kind)
    {
        PWSTR name;

        try
        {
            item->GetDisplayName(kind, &name);
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            return null;
        }

        try
        {
            return name.ToString();
        }
        finally
        {
            PInvoke.CoTaskMemFree(name.Value);
        }
    }

    private static byte[]? Render(string parsingName, int size)
    {
        IShellItemImageFactory* factory = null;
        HBITMAP bitmap = default;

        try
        {
            if (PInvoke.SHCreateItemFromParsingName(AppsFolder + "\\" + parsingName, null, out factory).Failed)
            {
                return null;
            }

            factory->GetImage(new SIZE(size, size), SIIGBF.SIIGBF_ICONONLY, &bitmap);
            return Encode(bitmap);
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            return null;
        }
        finally
        {
            if (!bitmap.IsNull)
            {
                PInvoke.DeleteObject((HGDIOBJ)bitmap.Value);
            }

            if (factory is not null)
            {
                factory->Release();
            }
        }
    }

    private static byte[]? Encode(HBITMAP bitmap)
    {
        BITMAP info;

        if (PInvoke.GetObject((HGDIOBJ)bitmap.Value, sizeof(BITMAP), &info) == 0)
        {
            return null;
        }

        var width = info.bmWidth;
        var height = Math.Abs(info.bmHeight);

        if (width is < 1 or > PngWriter.MaxDimension || height is < 1 or > PngWriter.MaxDimension)
        {
            return null;
        }

        var pixels = new byte[width * height * 4];
        var header = new BITMAPINFO();
        header.bmiHeader.biSize = (uint)sizeof(BITMAPINFOHEADER);
        header.bmiHeader.biWidth = width;
        header.bmiHeader.biHeight = -height;
        header.bmiHeader.biPlanes = 1;
        header.bmiHeader.biBitCount = 32;
        header.bmiHeader.biCompression = 0;

        var dc = PInvoke.CreateCompatibleDC(default);

        try
        {
            fixed (byte* target = pixels)
            {
                if (PInvoke.GetDIBits(dc, bitmap, 0, (uint)height, target, &header, DIB_USAGE.DIB_RGB_COLORS) != height)
                {
                    return null;
                }
            }
        }
        finally
        {
            PInvoke.DeleteDC(dc);
        }

        if (!HasAlpha(pixels))
        {
            for (var index = 3; index < pixels.Length; index += 4)
            {
                pixels[index] = 255;
            }
        }

        return PngWriter.EncodeBgra(pixels, width, height, PngWriter.LooksPremultiplied(pixels));
    }

    private static bool HasAlpha(ReadOnlySpan<byte> bgra)
    {
        for (var index = 3; index < bgra.Length; index += 4)
        {
            if (bgra[index] != 0)
            {
                return true;
            }
        }

        return false;
    }

    private void Run()
    {
        foreach (var item in _work.GetConsumingEnumerable())
        {
            try
            {
                item.Completion.TrySetResult(Handle(item.Request));
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                _log.Warn($"icon source failed: {exception.GetType().Name} 0x{exception.HResult:X8}");
                item.Completion.TrySetResult(null);
            }
        }
    }

    private byte[]? Handle(IconRequest request)
    {
        var parsingName = request.Origin == NotificationOrigin.Windows
            ? request.AppIdentifier
            : AppNameMatcher.FindBest(Index(), request.AppName)?.ParsingName;

        if (string.IsNullOrEmpty(parsingName) || parsingName.Length > MaxParsingNameLength || parsingName.Contains("://", StringComparison.Ordinal))
        {
            return null;
        }

        foreach (var size in Sizes)
        {
            if (Render(parsingName, size) is { } png && png.Length <= IslandProtocol.MaxIconBytes)
            {
                return png;
            }
        }

        return null;
    }

    private List<InstalledApp> Index()
    {
        if (_apps is not null && Environment.TickCount64 - _indexedAt < IndexLifetime.TotalMilliseconds)
        {
            return _apps;
        }

        _apps = Enumerate();
        _indexedAt = Environment.TickCount64;
        _log.Info($"icon index: {_apps.Count} apps");
        return _apps;
    }

    private List<InstalledApp> Enumerate()
    {
        List<InstalledApp> apps = [];
        IShellItem* folder = null;
        IEnumShellItems* items = null;

        try
        {
            if (PInvoke.SHCreateItemFromParsingName(AppsFolder, null, out folder).Failed)
            {
                return apps;
            }

            var handler = PInvoke.BHID_EnumItems;
            var iid = IEnumShellItems.IID_Guid;
            void* result;
            folder->BindToHandler(null, &handler, &iid, &result);
            items = (IEnumShellItems*)result;

            while (apps.Count < MaxApps)
            {
                IShellItem* item = null;
                uint fetched = 0;
                items->Next(1, &item, &fetched);

                if (fetched == 0 || item is null)
                {
                    break;
                }

                try
                {
                    var name = ReadName(item, SIGDN.SIGDN_NORMALDISPLAY);
                    var parsing = ReadName(item, SIGDN.SIGDN_PARENTRELATIVEPARSING);

                    if (!string.IsNullOrWhiteSpace(name) && !string.IsNullOrWhiteSpace(parsing))
                    {
                        apps.Add(new InstalledApp(name, parsing));
                    }
                }
                finally
                {
                    item->Release();
                }
            }
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _log.Warn($"icon index failed: {exception.GetType().Name} 0x{exception.HResult:X8}");
        }
        finally
        {
            if (items is not null)
            {
                items->Release();
            }

            if (folder is not null)
            {
                folder->Release();
            }
        }

        return apps;
    }

    private sealed record WorkItem(IconRequest Request, TaskCompletionSource<byte[]?> Completion);
}
