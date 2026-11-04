using IslandBridge.Core.Ancs;

namespace IslandBridge.Tests.Ancs;

public sealed class ResponseAssemblerTests
{
    [Fact]
    public void Reassembles_a_response_split_at_every_position()
    {
        var data = AncsPackets.NotificationResponse(9, message: "A message long enough to be split across packets");
        using var assembler = new ResponseAssembler();

        for (var cut = 1; cut < data.Length; cut++)
        {
            assembler.Begin(new ResponseExpectation.Notification(9));

            Assert.Equal(ParseStatus.NeedMoreData, assembler.Append(data.AsSpan(0, cut)).Status);
            var result = assembler.Append(data.AsSpan(cut));

            Assert.Equal(ParseStatus.Complete, result.Status);
            Assert.Equal("A message long enough to be split across packets", result.Notification!.Message);
            Assert.False(assembler.IsWaiting);
        }
    }

    [Fact]
    public void Reassembles_a_response_split_into_single_bytes()
    {
        var data = AncsPackets.AppResponse("com.example.app", "Example");
        using var assembler = new ResponseAssembler();
        assembler.Begin(new ResponseExpectation.App("com.example.app"));
        AssembledResponse result = default;

        foreach (var value in data)
        {
            result = assembler.Append([value]);
        }

        Assert.Equal(ParseStatus.Complete, result.Status);
        Assert.Equal("Example", result.DisplayName);
        Assert.Equal(data.Length, assembler.Fragments);
    }

    [Fact]
    public void Rejects_a_response_that_exceeds_the_buffer()
    {
        using var assembler = new ResponseAssembler();
        assembler.Begin(new ResponseExpectation.Notification(1));
        byte[] header = [0, 1, 0, 0, 0, 0, 0xFF, 0x07];

        Assert.Equal(ParseStatus.NeedMoreData, assembler.Append(header).Status);
        Assert.Equal(ParseStatus.Invalid, assembler.Append(new byte[AncsLimits.MaxResponseBytes]).Status);
        Assert.False(assembler.IsWaiting);
    }

    [Fact]
    public void Ignores_data_when_nothing_is_expected()
    {
        using var assembler = new ResponseAssembler();

        Assert.Equal(ParseStatus.Invalid, assembler.Append([0, 1, 2]).Status);
    }

    [Fact]
    public void Begin_discards_a_partial_previous_response()
    {
        var first = AncsPackets.NotificationResponse(1);
        var second = AncsPackets.NotificationResponse(2, title: "Second");
        using var assembler = new ResponseAssembler();

        assembler.Begin(new ResponseExpectation.Notification(1));
        assembler.Append(first.AsSpan(0, 10));
        assembler.Begin(new ResponseExpectation.Notification(2));
        var result = assembler.Append(second);

        Assert.Equal(ParseStatus.Complete, result.Status);
        Assert.Equal("Second", result.Notification!.Title);
    }
}
