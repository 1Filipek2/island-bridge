using IslandBridge.Core.Ancs;
using Microsoft.Extensions.Time.Testing;

namespace IslandBridge.Tests.Ancs;

public sealed class AncsRequesterTests
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task Returns_attributes_delivered_in_fragments()
    {
        var link = new FakeAncsLink
        {
            Responder = _ => (LinkResult.Success, AncsPackets.Split(AncsPackets.NotificationResponse(3, title: "Hi"), 4, 12, 20)),
        };
        using var requester = Create(link, new FakeTimeProvider());

        var result = await requester.GetNotificationAttributesAsync(3, TestContext.Current.CancellationToken);

        Assert.True(result.IsSuccess);
        Assert.Equal("Hi", result.Value!.Title);
        Assert.Equal(AncsCommands.GetNotificationAttributes(3, AncsCommands.DefaultNotificationAttributes), link.Writes.Single());
    }

    [Fact]
    public async Task Response_arriving_after_the_write_completes_is_accepted()
    {
        var link = new FakeAncsLink();
        using var requester = Create(link, new FakeTimeProvider());

        var pending = requester.GetAppDisplayNameAsync("com.example", TestContext.Current.CancellationToken);
        link.RaiseDataSource(AncsPackets.AppResponse("com.example", "Example"));
        var result = await pending;

        Assert.True(result.IsSuccess);
        Assert.Equal("Example", result.Value);
    }

    [Fact]
    public async Task Unknown_uid_error_is_reported_as_unknown_notification()
    {
        var link = new FakeAncsLink
        {
            Responder = _ => (new LinkResult(LinkStatus.ProtocolError, AncsErrorCodes.InvalidParameter), []),
        };
        using var requester = Create(link, new FakeTimeProvider());

        var result = await requester.GetNotificationAttributesAsync(3, TestContext.Current.CancellationToken);

        Assert.Equal(RequestStatus.UnknownNotification, result.Status);
    }

    [Fact]
    public async Task Other_write_failures_are_link_errors()
    {
        var link = new FakeAncsLink
        {
            Responder = _ => (new LinkResult(LinkStatus.Unreachable), []),
        };
        using var requester = Create(link, new FakeTimeProvider());

        var result = await requester.GetNotificationAttributesAsync(3, TestContext.Current.CancellationToken);

        Assert.Equal(RequestStatus.LinkError, result.Status);
        Assert.Equal(LinkStatus.Unreachable, result.Link.Status);
    }

    [Fact]
    public async Task Times_out_when_the_response_never_completes()
    {
        var time = new FakeTimeProvider();
        var link = new FakeAncsLink
        {
            Responder = _ => (LinkResult.Success, [AncsPackets.NotificationResponse(3)[..6]]),
        };
        using var requester = Create(link, time);

        var pending = requester.GetNotificationAttributesAsync(3, TestContext.Current.CancellationToken);
        time.Advance(Timeout + TimeSpan.FromMilliseconds(1));
        var result = await pending;

        Assert.Equal(RequestStatus.Timeout, result.Status);
    }

    [Fact]
    public async Task Late_fragments_after_a_timeout_do_not_leak_into_the_next_request()
    {
        var time = new FakeTimeProvider();
        var link = new FakeAncsLink();
        using var requester = Create(link, time);

        var first = requester.GetNotificationAttributesAsync(1, TestContext.Current.CancellationToken);
        time.Advance(Timeout + TimeSpan.FromMilliseconds(1));
        Assert.Equal(RequestStatus.Timeout, (await first).Status);

        link.RaiseDataSource(AncsPackets.NotificationResponse(1));
        Assert.Equal(1, requester.UnexpectedPackets);

        link.Responder = _ => (LinkResult.Success, [AncsPackets.NotificationResponse(2, title: "Second")]);
        var second = await requester.GetNotificationAttributesAsync(2, TestContext.Current.CancellationToken);

        Assert.Equal("Second", second.Value!.Title);
    }

    [Fact]
    public async Task Invalid_response_is_reported()
    {
        var link = new FakeAncsLink
        {
            Responder = _ => (LinkResult.Success, [AncsPackets.NotificationResponse(99)]),
        };
        using var requester = Create(link, new FakeTimeProvider());

        var result = await requester.GetNotificationAttributesAsync(3, TestContext.Current.CancellationToken);

        Assert.Equal(RequestStatus.InvalidResponse, result.Status);
    }

    [Fact]
    public async Task Cancel_pending_completes_the_request_as_link_lost()
    {
        var link = new FakeAncsLink();
        using var requester = Create(link, new FakeTimeProvider());

        var pending = requester.GetNotificationAttributesAsync(3, TestContext.Current.CancellationToken);
        requester.CancelPending();
        var result = await pending;

        Assert.Equal(RequestStatus.LinkLost, result.Status);
    }

    [Fact]
    public async Task Requests_are_serialized()
    {
        var link = new FakeAncsLink();
        using var requester = Create(link, new FakeTimeProvider());

        var first = requester.GetNotificationAttributesAsync(1, TestContext.Current.CancellationToken);
        var second = requester.GetNotificationAttributesAsync(2, TestContext.Current.CancellationToken);

        Assert.Single(link.Writes);

        link.RaiseDataSource(AncsPackets.NotificationResponse(1));
        Assert.True((await first).IsSuccess);

        var deadline = DateTime.UtcNow.AddSeconds(10);

        while (link.Writes.Count < 2 && DateTime.UtcNow < deadline)
        {
            await Task.Delay(5, TestContext.Current.CancellationToken);
        }

        link.RaiseDataSource(AncsPackets.NotificationResponse(2));
        Assert.True((await second).IsSuccess);
        Assert.Equal(2, link.Writes.Count);
    }

    [Fact]
    public void Data_without_a_request_is_counted_and_dropped()
    {
        using var requester = Create(new FakeAncsLink(), new FakeTimeProvider());

        requester.OnDataSource([0, 1, 2, 3]);

        Assert.Equal(1, requester.UnexpectedPackets);
    }

    private static AncsRequester Create(FakeAncsLink link, TimeProvider time)
    {
        var requester = new AncsRequester(link, time, Timeout);
        link.DataSourceReceived += requester.OnDataSource;
        return requester;
    }
}
