using IslandBridge.Core.Ancs;

namespace IslandBridge.Core.Notifications;

public enum NotificationOrigin
{
    Iphone,
    Windows,
}

public sealed record IslandNotification(
    uint Uid,
    AncsCategory Category,
    AncsEventFlags Flags,
    string AppIdentifier,
    string AppName,
    string Title,
    string Subtitle,
    string Message,
    DateTime? Date,
    long ReceivedTimestamp,
    NotificationOrigin Origin = NotificationOrigin.Iphone);
