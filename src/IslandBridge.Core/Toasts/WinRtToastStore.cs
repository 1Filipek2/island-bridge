using Windows.UI.Notifications;
using Windows.UI.Notifications.Management;

namespace IslandBridge.Core.Toasts;

public sealed class WinRtToastStore : IToastStore
{
    private readonly UserNotificationListener _listener = UserNotificationListener.Current;

    public async Task<ToastAccess> RequestAccessAsync(CancellationToken cancellationToken)
    {
        var status = _listener.GetAccessStatus();

        if (status == UserNotificationListenerAccessStatus.Unspecified)
        {
            try
            {
                status = await _listener.RequestAccessAsync().AsTask(cancellationToken).ConfigureAwait(false);
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                return ToastAccess.Unavailable;
            }
        }

        return status switch
        {
            UserNotificationListenerAccessStatus.Allowed => ToastAccess.Allowed,
            UserNotificationListenerAccessStatus.Denied => ToastAccess.Denied,
            _ => ToastAccess.Unavailable,
        };
    }

    public async Task<IReadOnlyList<uint>> ListIdsAsync(CancellationToken cancellationToken)
    {
        var notifications = await _listener.GetNotificationsAsync(NotificationKinds.Toast).AsTask(cancellationToken).ConfigureAwait(false);
        var ids = new uint[notifications.Count];

        for (var index = 0; index < ids.Length; index++)
        {
            ids[index] = notifications[index].Id;
        }

        return ids;
    }

    public ToastDetails? GetDetails(uint id)
    {
        var notification = _listener.GetNotification(id);

        if (notification is null)
        {
            return null;
        }

        var (appUserModelId, displayName) = ReadAppInfo(notification);
        var binding = notification.Notification?.Visual?.GetBinding(KnownNotificationBindings.ToastGeneric);
        List<string> texts = [];

        if (binding is not null)
        {
            foreach (var element in binding.GetTextElements())
            {
                if (texts.Count == ToastText.MaxTextElements)
                {
                    break;
                }

                texts.Add(element.Text ?? string.Empty);
            }
        }

        return new ToastDetails(id, appUserModelId, displayName, texts, notification.CreationTime);
    }

    private static (string AppUserModelId, string DisplayName) ReadAppInfo(UserNotification notification)
    {
        try
        {
            var appInfo = notification.AppInfo;
            return appInfo is null ? (string.Empty, string.Empty) : (appInfo.AppUserModelId ?? string.Empty, appInfo.DisplayInfo?.DisplayName ?? string.Empty);
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            return (string.Empty, string.Empty);
        }
    }
}
