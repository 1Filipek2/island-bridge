using System.Diagnostics;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Windows.Graphics;
using Windows.Win32;

namespace IslandBridge;

internal readonly record struct ShowRequest(PointInt32? Anchor, bool Toggle, long RequestedAt);

public partial class App : Application
{
    private static readonly TimeSpan DismissDebounce = TimeSpan.FromMilliseconds(400);

    private readonly AppHost _host;
    private readonly ShowRequest _initialRequest;
    private MainWindow? _window;
    private long _dismissedAt;

    internal App(AppHost host, ShowRequest initialRequest)
    {
        _host = host;
        _initialRequest = initialRequest;
        InitializeComponent();
        DispatcherShutdownMode = DispatcherShutdownMode.OnExplicitShutdown;
    }

    internal void Show(ShowRequest request)
    {
        if (_window is { } existing)
        {
            if (request.Toggle && !existing.IsMinimized)
            {
                existing.Close();
            }
            else
            {
                existing.BringToFront();
            }

            return;
        }

        if (request.Toggle && _dismissedAt != 0 && Stopwatch.GetElapsedTime(_dismissedAt) < DismissDebounce)
        {
            return;
        }

        _window = new MainWindow(_host, request.Anchor, request.RequestedAt);
        _window.Closed += OnWindowClosed;
        _window.BringToFront();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args) => Show(_initialRequest);

    private static void TrimWorkingSet()
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        PInvoke.K32EmptyWorkingSet(PInvoke.GetCurrentProcess());
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        if (_window is { } window)
        {
            window.Closed -= OnWindowClosed;
            _dismissedAt = window.IsLightDismissed ? Stopwatch.GetTimestamp() : 0;
            _window = null;
        }

        DispatcherQueue.GetForCurrentThread().TryEnqueue(DispatcherQueuePriority.Low, TrimWorkingSet);
    }
}
