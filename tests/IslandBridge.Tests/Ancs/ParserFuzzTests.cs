using IslandBridge.Core.Ancs;
using IslandBridge.Core.Text;

namespace IslandBridge.Tests.Ancs;

public sealed class ParserFuzzTests
{
    private const int Iterations = 20_000;

    [Fact]
    public void Random_bytes_never_throw()
    {
        var random = new Random(1234);
        using var assembler = new ResponseAssembler();

        for (var i = 0; i < Iterations; i++)
        {
            var data = new byte[random.Next(0, 600)];
            random.NextBytes(data);

            if (data.Length > 0 && random.Next(2) == 0)
            {
                data[0] = (byte)random.Next(0, 3);
            }

            _ = NotificationSourceEvent.TryParse(data, out _);
            AttributeResponseParser.ParseNotification(data, (uint)random.Next(), AncsCommands.DefaultNotificationAttributes, out _);
            AttributeResponseParser.ParseApp(data, "com.example"u8, out _);
            TextSanitizer.Sanitize(data, 64);

            if (!assembler.IsWaiting)
            {
                assembler.Begin(random.Next(2) == 0
                    ? new ResponseExpectation.Notification((uint)random.Next(4))
                    : new ResponseExpectation.App("com.example"));
            }

            assembler.Append(data);
        }
    }

    [Fact]
    public void Mutated_valid_responses_never_throw_and_never_complete_with_wrong_uid()
    {
        var random = new Random(5678);
        var valid = AncsPackets.NotificationResponse(77, message: "Mutation target with some length");

        for (var i = 0; i < Iterations; i++)
        {
            var data = (byte[])valid.Clone();
            var mutations = random.Next(1, 6);

            for (var m = 0; m < mutations; m++)
            {
                data[random.Next(data.Length)] = (byte)random.Next(256);
            }

            var cut = random.Next(data.Length + 1);
            var status = AttributeResponseParser.ParseNotification(data.AsSpan(0, cut), 77, AncsCommands.DefaultNotificationAttributes, out var attributes);

            Assert.Equal(status == ParseStatus.Complete, attributes is not null);

            if (status == ParseStatus.Complete)
            {
                Assert.True(attributes!.Title.Length <= AncsLimits.TitleMaxLength);
                Assert.True(attributes.Message.Length <= AncsLimits.MessageMaxLength);
                Assert.DoesNotContain(attributes.Message, character => char.IsControl(character));
            }
        }
    }

    [Fact]
    public void Random_fragmentation_always_reassembles_the_same_result()
    {
        var random = new Random(91011);
        var data = AncsPackets.NotificationResponse(5, title: "Fragmented", message: new string('x', 300));
        using var assembler = new ResponseAssembler();

        for (var i = 0; i < 2_000; i++)
        {
            assembler.Begin(new ResponseExpectation.Notification(5));
            var offset = 0;
            AssembledResponse result = default;

            while (offset < data.Length)
            {
                var size = Math.Min(random.Next(1, 40), data.Length - offset);
                result = assembler.Append(data.AsSpan(offset, size));
                offset += size;

                if (offset < data.Length)
                {
                    Assert.Equal(ParseStatus.NeedMoreData, result.Status);
                }
            }

            Assert.Equal(ParseStatus.Complete, result.Status);
            Assert.Equal("Fragmented", result.Notification!.Title);
        }
    }
}
