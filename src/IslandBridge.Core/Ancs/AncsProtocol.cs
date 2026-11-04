namespace IslandBridge.Core.Ancs;

public enum NotificationAttributeId : byte
{
    AppIdentifier = 0,
    Title = 1,
    Subtitle = 2,
    Message = 3,
    MessageSize = 4,
    Date = 5,
    PositiveActionLabel = 6,
    NegativeActionLabel = 7,
}

public enum AncsCommandId : byte
{
    GetNotificationAttributes = 0,
    GetAppAttributes = 1,
    PerformNotificationAction = 2,
}

public static class AncsLimits
{
    public const ushort TitleMaxLength = 128;
    public const ushort SubtitleMaxLength = 128;
    public const ushort MessageMaxLength = 512;
    public const int MaxAttributeBytes = 2048;
    public const int MaxAppIdentifierBytes = 256;
    public const int MaxResponseBytes = 8 * 1024;
    public const int MaxDisplayNameLength = 64;
    public const int MaxAppIdentifierLength = 128;
}

public static class AncsErrorCodes
{
    public const byte UnknownCommand = 0xA0;
    public const byte InvalidCommand = 0xA1;
    public const byte InvalidParameter = 0xA2;
    public const byte ActionFailed = 0xA3;
    public const byte InsufficientAuthentication = 0x05;
    public const byte InsufficientAuthorization = 0x08;
    public const byte InsufficientEncryptionKeySize = 0x0C;
    public const byte InsufficientEncryption = 0x0F;

    public static bool IsPermissionError(byte code) =>
        code is InsufficientAuthentication or InsufficientAuthorization or InsufficientEncryptionKeySize or InsufficientEncryption;
}
