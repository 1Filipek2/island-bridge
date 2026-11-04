using IslandBridge.Core.Ancs;

namespace IslandBridge.Tests.Ancs;

public sealed class NotificationSourceEventTests
{
    [Fact]
    public void Parses_a_valid_packet()
    {
        var packet = AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.Important, AncsCategory.Social, 0x01020304, 3);

        Assert.True(NotificationSourceEvent.TryParse(packet, out var result));
        Assert.Equal(AncsEventId.Added, result.EventId);
        Assert.Equal(AncsEventFlags.Important, result.Flags);
        Assert.Equal(AncsCategory.Social, result.Category);
        Assert.Equal(3, result.CategoryCount);
        Assert.Equal(0x01020304u, result.Uid);
        Assert.True(result.IsNew);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(7)]
    [InlineData(9)]
    [InlineData(20)]
    public void Rejects_wrong_length(int length)
    {
        Assert.False(NotificationSourceEvent.TryParse(new byte[length], out _));
    }

    [Fact]
    public void Rejects_unknown_event_id()
    {
        var packet = AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Other, 1);
        packet[0] = 3;

        Assert.False(NotificationSourceEvent.TryParse(packet, out _));
    }

    [Fact]
    public void Maps_unknown_category_to_other_and_masks_unknown_flags()
    {
        var packet = AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.None, AncsCategory.Other, 1);
        packet[1] = 0xFF;
        packet[2] = 200;

        Assert.True(NotificationSourceEvent.TryParse(packet, out var result));
        Assert.Equal(AncsCategory.Other, result.Category);
        Assert.Equal(0x1F, (byte)result.Flags);
    }

    [Fact]
    public void Pre_existing_notifications_are_not_new()
    {
        var packet = AncsPackets.NotificationSource(AncsEventId.Added, AncsEventFlags.PreExisting | AncsEventFlags.Silent, AncsCategory.Other, 1);

        Assert.True(NotificationSourceEvent.TryParse(packet, out var result));
        Assert.False(result.IsNew);
    }

    [Theory]
    [InlineData(AncsEventId.Modified)]
    [InlineData(AncsEventId.Removed)]
    public void Only_added_events_are_new(AncsEventId eventId)
    {
        var packet = AncsPackets.NotificationSource(eventId, AncsEventFlags.None, AncsCategory.Other, 1);

        Assert.True(NotificationSourceEvent.TryParse(packet, out var result));
        Assert.False(result.IsNew);
    }
}
