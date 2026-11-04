using System.Buffers.Binary;
using System.Text;
using IslandBridge.Core.Ancs;
using Microsoft.Extensions.Time.Testing;

namespace IslandBridge.Tests.Ancs;

public sealed class AncsClientTests
{
    private static readonly TimeSpan WaitLimit = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task New_notification_is_fetched_named_and_published()
    {
        await using var harness = new Harness();
        await harness.ReadyAsync();

        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Social, 10));
        var notification = await harness.NextAsync();

        Assert.Equal(10u, notification.Uid);
        Assert.Equal(AncsCategory.Social, notification.Category);
        Assert.Equal("net.whatsapp.WhatsApp", notification.AppIdentifier);
        Assert.Equal("WhatsApp", notification.AppName);
        Assert.Equal("Alice", notification.Title);
        Assert.Equal("Hello there", notification.Message);
        Assert.True(harness.Cache.TryGet("net.whatsapp.WhatsApp", out _));
    }

    [Fact]
    public async Task App_name_comes_from_the_cache_on_the_second_notification()
    {
        await using var harness = new Harness();
        await harness.ReadyAsync();

        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Social, 1));
        await harness.NextAsync();
        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Social, 2));
        await harness.NextAsync();

        Assert.Equal(1, harness.Link.Writes.Count(write => write[0] == (byte)AncsCommandId.GetAppAttributes));
    }

    [Fact]
    public async Task Pre_existing_modified_and_malformed_events_are_ignored()
    {
        await using var harness = new Harness();
        await harness.ReadyAsync();

        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.PreExisting, AncsCategory.Other, 1));
        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Modified, AncsEventFlags.None, AncsCategory.Other, 2));
        harness.Link.RaiseNotificationSource([1, 2, 3]);
        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Email, 3));
        var notification = await harness.NextAsync();

        Assert.Equal(3u, notification.Uid);
        Assert.Equal(1, harness.Client.MalformedEvents);
    }

    [Fact]
    public async Task Removed_before_fetch_is_skipped()
    {
        await using var harness = new Harness();
        await harness.ReadyAsync();
        harness.Link.Responder = null;

        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Other, 1));
        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Other, 2));
        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Removed, AncsEventFlags.None, AncsCategory.Other, 2));
        harness.Link.Responder = Harness.Respond;
        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Other, 3));
        await WaitUntilAsync(() => harness.Link.Writes.Count >= 1);
        harness.Link.RaiseDataSource(AncsPackets.NotificationResponse(1));

        var first = await harness.NextAsync();
        var second = await harness.NextAsync();

        Assert.Equal(1u, first.Uid);
        Assert.Equal(3u, second.Uid);
        Assert.DoesNotContain(harness.Link.Writes, write => write[0] == 0 && BinaryPrimitives.ReadUInt32LittleEndian(write.AsSpan(1)) == 2);
    }

    [Fact]
    public async Task Unknown_app_name_falls_back_to_the_identifier()
    {
        await using var harness = new Harness(command => command[0] == (byte)AncsCommandId.GetAppAttributes
            ? (new LinkResult(LinkStatus.ProtocolError, AncsErrorCodes.InvalidParameter), [])
            : Harness.Respond(command));
        await harness.ReadyAsync();

        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Other, 1));
        var notification = await harness.NextAsync();

        Assert.Equal("net.whatsapp.WhatsApp", notification.AppName);
        Assert.False(harness.Cache.TryGet("net.whatsapp.WhatsApp", out _));
    }

    [Fact]
    public async Task Notification_that_vanished_on_the_phone_is_dropped_quietly()
    {
        await using var harness = new Harness(command => command[0] == (byte)AncsCommandId.GetNotificationAttributes
            && BinaryPrimitives.ReadUInt32LittleEndian(command.AsSpan(1)) == 1
                ? (new LinkResult(LinkStatus.ProtocolError, AncsErrorCodes.InvalidParameter), [])
                : Harness.Respond(command));
        await harness.ReadyAsync();

        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Other, 1));
        harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Other, 2));
        var notification = await harness.NextAsync();

        Assert.Equal(2u, notification.Uid);
        Assert.Equal(0, harness.Client.FailedRequests);
    }

    [Fact]
    public async Task Flood_is_bounded()
    {
        await using var harness = new Harness(options: new AncsClientOptions { EventQueueCapacity = 4, OutputQueueCapacity = 4 });
        await harness.ReadyAsync();
        harness.Link.Responder = null;

        for (uint uid = 1; uid <= 1000; uid++)
        {
            harness.Link.RaiseNotificationSource(AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Other, uid));
        }

        await WaitUntilAsync(() => harness.Link.Writes.Count >= 1);

        Assert.Single(harness.Link.Writes);
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + WaitLimit;

        while (!condition())
        {
            Assert.True(DateTime.UtcNow < deadline, "condition not met in time");
            await Task.Delay(5);
        }
    }

    private sealed class Harness : IAsyncDisposable
    {
        private readonly CancellationTokenSource _cancellation = new();
        private readonly TaskCompletionSource _ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly Task _run;

        public Harness(
            Func<byte[], (LinkResult Result, IEnumerable<byte[]> Fragments)>? responder = null,
            AncsClientOptions? options = null)
        {
            Link.Responder = responder ?? Respond;
            Client = new AncsClient(Link, Cache, new FakeTimeProvider(), ReconnectBackoff.Default, options);
            Client.Supervisor.StateChanged += (_, args) =>
            {
                if (args.State == ConnectionState.Ready)
                {
                    _ready.TrySetResult();
                }
            };
            _run = Task.Run(() => Client.RunAsync(_cancellation.Token));
        }

        public FakeAncsLink Link { get; } = new();

        public AppNameCache Cache { get; } = new();

        public AncsClient Client { get; }

        public static (LinkResult Result, IEnumerable<byte[]> Fragments) Respond(byte[] command)
        {
            if (command[0] == (byte)AncsCommandId.GetAppAttributes)
            {
                var identifier = Encoding.UTF8.GetString(command.AsSpan(1, command.Length - 3));
                return (LinkResult.Success, [AncsPackets.AppResponse(identifier, "WhatsApp")]);
            }

            var uid = BinaryPrimitives.ReadUInt32LittleEndian(command.AsSpan(1));
            return (LinkResult.Success, AncsPackets.Split(AncsPackets.NotificationResponse(uid), 20));
        }

        public Task ReadyAsync() => _ready.Task.WaitAsync(WaitLimit);

        public async Task<PhoneNotification> NextAsync()
        {
            using var timeout = new CancellationTokenSource(WaitLimit);
            return await Client.Notifications.ReadAsync(timeout.Token);
        }

        public async ValueTask DisposeAsync()
        {
            await _cancellation.CancelAsync();

            try
            {
                await _run;
            }
            catch (OperationCanceledException)
            {
            }

            _cancellation.Dispose();
            Client.Dispose();
        }
    }
}
