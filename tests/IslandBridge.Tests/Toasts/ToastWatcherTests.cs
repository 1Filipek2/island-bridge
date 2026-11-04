using IslandBridge.Core.Ancs;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Notifications;
using IslandBridge.Core.Toasts;

namespace IslandBridge.Tests.Toasts;

public sealed class ToastWatcherTests
{
    private static readonly TimeSpan WaitLimit = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task Existing_toasts_are_a_baseline_and_only_new_ones_are_published()
    {
        await using var harness = await Harness.StartAsync(store => store.Add(Toast(1, "Old")));

        harness.Store.Add(Toast(2, "New"));
        harness.Signal.Fire();

        var notification = await harness.NextAsync();
        Assert.Equal(2u, notification.Uid);
        Assert.Equal("New", notification.Title);
        Assert.Equal(NotificationOrigin.Windows, notification.Origin);
        Assert.Equal(ToastWatcherState.Running, harness.Watcher.State);
    }

    [Fact]
    public async Task A_signal_without_new_toasts_publishes_nothing()
    {
        await using var harness = await Harness.StartAsync(store => store.Add(Toast(1, "Old")));

        harness.Signal.Fire();
        harness.Store.Add(Toast(2, "Marker"));
        harness.Signal.Fire();

        Assert.Equal(2u, (await harness.NextAsync()).Uid);
        Assert.False(harness.Watcher.Notifications.TryRead(out _));
    }

    [Fact]
    public async Task Removed_toasts_leave_the_seen_set()
    {
        await using var harness = await Harness.StartAsync(store => store.Add(Toast(1, "Old")));

        harness.Store.Remove(1);
        harness.Store.Add(Toast(2, "Second"));
        harness.Signal.Fire();
        Assert.Equal(2u, (await harness.NextAsync()).Uid);

        harness.Store.Add(Toast(1, "Back"));
        harness.Signal.Fire();
        Assert.Equal(1u, (await harness.NextAsync()).Uid);
    }

    [Fact]
    public async Task Signals_are_coalesced_while_a_scan_runs()
    {
        await using var harness = await Harness.StartAsync();
        harness.Store.BlockListing();

        harness.Signal.Fire();
        await harness.Store.WaitForBlockedListAsync();

        for (var index = 0; index < 50; index++)
        {
            harness.Signal.Fire();
        }

        harness.Store.Add(Toast(5, "After"));
        var listsBefore = harness.Store.ListCalls;
        harness.Store.ReleaseListing();

        Assert.Equal(5u, (await harness.NextAsync()).Uid);
        Assert.InRange(harness.Store.ListCalls - listsBefore, 1, 2);
    }

    [Fact]
    public async Task Phone_link_toasts_are_skipped()
    {
        await using var harness = await Harness.StartAsync();

        harness.Store.Add(Toast(1, "Duplicate", appId: ToastWatcher.PhoneLinkAppId));
        harness.Store.Add(Toast(2, "Kept"));
        harness.Signal.Fire();

        Assert.Equal(2u, (await harness.NextAsync()).Uid);
    }

    [Fact]
    public async Task Detail_failures_are_counted_and_do_not_stop_the_watcher()
    {
        await using var harness = await Harness.StartAsync();

        harness.Store.Add(Toast(1, "Broken"));
        harness.Store.FailDetailsFor(1);
        harness.Store.Add(Toast(2, "Fine"));
        harness.Signal.Fire();

        Assert.Equal(2u, (await harness.NextAsync()).Uid);
        Assert.Equal(1, harness.Watcher.FailedDetails);
    }

    [Fact]
    public async Task List_failure_keeps_the_previous_baseline()
    {
        await using var harness = await Harness.StartAsync(store => store.Add(Toast(1, "Old")));

        harness.Store.FailNextList();
        harness.Signal.Fire();
        await harness.Store.WaitForFailedListAsync();
        harness.Store.Add(Toast(2, "New"));
        harness.Signal.Fire();

        Assert.Equal(2u, (await harness.NextAsync()).Uid);
        Assert.Equal(1, harness.Watcher.FailedScans);
    }

    [Fact]
    public async Task Bursts_are_capped_to_the_newest_toasts()
    {
        await using var harness = await Harness.StartAsync();

        for (uint id = 1; id <= ToastWatcher.MaxDetailsPerScan + 4; id++)
        {
            harness.Store.Add(Toast(id, $"T{id}"));
        }

        harness.Signal.Fire();

        Assert.Equal(5u, (await harness.NextAsync()).Uid);
    }

    [Fact]
    public async Task Windows_apps_are_added_to_the_app_name_cache()
    {
        await using var harness = await Harness.StartAsync();

        harness.Store.Add(Toast(1, "Hi", appId: "Contoso.Chat!App", name: "Contoso Chat"));
        harness.Store.Add(Toast(2, "Hi", appId: "Unknown!App", name: string.Empty));
        harness.Signal.Fire();

        await harness.NextAsync();
        await harness.NextAsync();
        Assert.True(harness.AppNames.TryGet("Contoso.Chat!App", out var name));
        Assert.Equal("Contoso Chat", name);
        Assert.False(harness.AppNames.TryGet("Unknown!App", out _));
    }

    [Fact]
    public async Task Denied_access_stops_without_subscribing()
    {
        var store = new FakeToastStore { Access = ToastAccess.Denied };
        using var signal = new FakeToastSignal();
        var watcher = new ToastWatcher(signal, store, new AppNameCache(), NullLog.Instance);

        await watcher.RunAsync(TestContext.Current.CancellationToken).WaitAsync(WaitLimit, TestContext.Current.CancellationToken);

        Assert.Equal(ToastWatcherState.AccessDenied, watcher.State);
        Assert.False(signal.Started);
        Assert.True(signal.Disposed);
    }

    [Fact]
    public async Task Signal_failure_reports_unavailable()
    {
        var store = new FakeToastStore();
        using var signal = new FakeToastSignal { CanStart = false };
        var watcher = new ToastWatcher(signal, store, new AppNameCache(), NullLog.Instance);

        await watcher.RunAsync(TestContext.Current.CancellationToken).WaitAsync(WaitLimit, TestContext.Current.CancellationToken);

        Assert.Equal(ToastWatcherState.Unavailable, watcher.State);
        Assert.Equal(0, store.ListCalls);
    }

    [Fact]
    public async Task Cancellation_stops_and_disposes_the_signal()
    {
        var harness = await Harness.StartAsync();

        await harness.DisposeAsync();

        Assert.Equal(ToastWatcherState.Stopped, harness.Watcher.State);
        Assert.True(harness.Signal.Disposed);
    }

    private static ToastDetails Toast(uint id, string title, string appId = "Contoso.App!App", string name = "Contoso") =>
        new(id, appId, name, [title, "Body"], DateTimeOffset.UnixEpoch);

    private sealed class Harness : IAsyncDisposable
    {
        private readonly CancellationTokenSource _cancellation = new();
        private Task _run = Task.CompletedTask;

        private Harness()
        {
            Watcher = new ToastWatcher(Signal, Store, AppNames, NullLog.Instance);
        }

        public FakeToastStore Store { get; } = new();

        public FakeToastSignal Signal { get; } = new();

        public AppNameCache AppNames { get; } = new();

        public ToastWatcher Watcher { get; }

        public static async Task<Harness> StartAsync(Action<FakeToastStore>? seed = null)
        {
            var harness = new Harness();
            seed?.Invoke(harness.Store);
            var running = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            harness.Watcher.StateChanged += (_, _) =>
            {
                if (harness.Watcher.State == ToastWatcherState.Running)
                {
                    running.TrySetResult();
                }
            };
            harness._run = Task.Run(() => harness.Watcher.RunAsync(harness._cancellation.Token));
            await running.Task.WaitAsync(WaitLimit);
            return harness;
        }

        public async Task<IslandNotification> NextAsync()
        {
            using var timeout = new CancellationTokenSource(WaitLimit);
            return await Watcher.Notifications.ReadAsync(timeout.Token);
        }

        public async ValueTask DisposeAsync()
        {
            Store.ReleaseListing();
            await _cancellation.CancelAsync();
            await _run.WaitAsync(WaitLimit);
            _cancellation.Dispose();
            Signal.Dispose();
        }
    }
}
