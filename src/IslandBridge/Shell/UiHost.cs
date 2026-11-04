using System.Diagnostics;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Windows.Graphics;

namespace IslandBridge.Shell;

internal sealed class UiHost(AppHost host)
{
    private readonly Lock _gate = new();
    private Thread? _thread;
    private DispatcherQueue? _dispatcher;

    public void Show(PointInt32? anchor = null, bool toggle = false)
    {
        var request = new ShowRequest(anchor, toggle, Stopwatch.GetTimestamp());
        DispatcherQueue? dispatcher;

        lock (_gate)
        {
            if (_thread is null)
            {
                _thread = new Thread(() => Run(request)) { Name = "IslandBridge UI", IsBackground = true };
                _thread.SetApartmentState(ApartmentState.STA);
                _thread.Start();
                return;
            }

            dispatcher = _dispatcher;
        }

        dispatcher?.TryEnqueue(() => (Application.Current as App)?.Show(request));
    }

    public void Shutdown()
    {
        Thread? thread;
        DispatcherQueue? dispatcher;

        lock (_gate)
        {
            thread = _thread;
            dispatcher = _dispatcher;
        }

        if (thread is null || dispatcher is null)
        {
            return;
        }

        dispatcher.TryEnqueue(static () => Application.Current.Exit());
        thread.Join(TimeSpan.FromSeconds(3));
    }

    private void Run(ShowRequest request)
    {
        Application.Start(parameters =>
        {
            var dispatcher = DispatcherQueue.GetForCurrentThread();
            SynchronizationContext.SetSynchronizationContext(new DispatcherQueueSynchronizationContext(dispatcher));

            lock (_gate)
            {
                _dispatcher = dispatcher;
            }

            _ = new App(host, request);
        });
    }
}
