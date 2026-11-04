using System.IO.Pipes;
using IslandBridge.Core.Ancs;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Island;
using IslandBridge.Core.Notifications;

namespace IslandBridge.Tests.Island;

public sealed class IslandPipeClientTests
{
    private static readonly TimeSpan WaitLimit = TimeSpan.FromSeconds(10);

    private readonly string _pipeName = "IslandBridge.Tests." + Guid.NewGuid().ToString("N");

    [Fact]
    public async Task Sends_each_notification_as_one_message()
    {
        await using var server = CreateServer();
        var accepted = server.WaitForConnectionAsync(TestContext.Current.CancellationToken);
        await using var client = new IslandPipeClient(NullLog.Instance, _ => true, _pipeName);
        using var cancellation = new CancellationTokenSource();
        var run = client.RunAsync(cancellation.Token);

        client.TrySend(Create("First"));
        await accepted.WaitAsync(WaitLimit, TestContext.Current.CancellationToken);
        var first = await ReadMessageAsync(server);
        client.TrySend(Create("Second"));
        var second = await ReadMessageAsync(server);

        Assert.Equal(IslandProtocol.Encode(Create("First")), first);
        Assert.Equal(IslandProtocol.Encode(Create("Second")), second);
        Assert.Equal(IslandLinkState.Connected, client.State);

        await cancellation.CancelAsync();
        await IgnoreCancellation(run);
    }

    [Fact]
    public async Task Refuses_an_untrusted_server()
    {
        await using var server = CreateServer();
        _ = server.WaitForConnectionAsync(TestContext.Current.CancellationToken);
        await using var client = new IslandPipeClient(NullLog.Instance, _ => false, _pipeName);
        using var cancellation = new CancellationTokenSource();
        var run = client.RunAsync(cancellation.Token);

        client.TrySend(Create("Secret"));
        await WaitUntilAsync(() => client.Dropped == 1);

        Assert.Equal(IslandLinkState.Untrusted, client.State);
        Assert.Equal(0, client.Sent);

        await cancellation.CancelAsync();
        await IgnoreCancellation(run);
    }

    [Fact]
    public async Task Drops_quietly_when_the_island_is_missing()
    {
        await using var client = new IslandPipeClient(NullLog.Instance, _ => true, _pipeName, TimeSpan.FromMilliseconds(50));
        using var cancellation = new CancellationTokenSource();
        var run = client.RunAsync(cancellation.Token);

        client.TrySend(Create("Nobody listens"));
        await WaitUntilAsync(() => client.Dropped == 1);

        Assert.Equal(IslandLinkState.Unavailable, client.State);

        await cancellation.CancelAsync();
        await IgnoreCancellation(run);
    }

    [Fact]
    public async Task Reconnects_after_the_server_restarts()
    {
        await using var client = new IslandPipeClient(NullLog.Instance, _ => true, _pipeName);
        using var cancellation = new CancellationTokenSource();
        var run = client.RunAsync(cancellation.Token);

        await using (var first = CreateServer())
        {
            var accepted = first.WaitForConnectionAsync(TestContext.Current.CancellationToken);
            client.TrySend(Create("One"));
            await accepted.WaitAsync(WaitLimit, TestContext.Current.CancellationToken);
            await ReadMessageAsync(first);
        }

        await using var second = CreateServer();
        var reaccepted = second.WaitForConnectionAsync(TestContext.Current.CancellationToken);
        client.TrySend(Create("Two"));
        await reaccepted.WaitAsync(WaitLimit, TestContext.Current.CancellationToken);
        var message = await ReadMessageAsync(second);

        Assert.Equal(IslandProtocol.Encode(Create("Two")), message);

        await cancellation.CancelAsync();
        await IgnoreCancellation(run);
    }

    private NamedPipeServerStream CreateServer() => new(
        _pipeName,
        PipeDirection.In,
        1,
        PipeTransmissionMode.Message,
        PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);

    private static IslandNotification Create(string title) =>
        new(1, AncsCategory.Social, AncsEventFlags.None, "com.example", "Example", title, string.Empty, "Body", null, 0);

    private static async Task<byte[]> ReadMessageAsync(NamedPipeServerStream server)
    {
        using var timeout = new CancellationTokenSource(WaitLimit);
        var buffer = new byte[IslandProtocol.MaxMessageBytes];
        var read = await server.ReadAsync(buffer, timeout.Token);

        Assert.True(server.IsMessageComplete);
        return buffer[..read];
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + WaitLimit;

        while (!condition())
        {
            Assert.True(DateTime.UtcNow < deadline, "condition not met in time");
            await Task.Delay(10);
        }
    }

    private static async Task IgnoreCancellation(Task task)
    {
        try
        {
            await task;
        }
        catch (OperationCanceledException)
        {
        }
    }
}
