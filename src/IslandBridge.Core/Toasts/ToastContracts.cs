namespace IslandBridge.Core.Toasts;

public enum ToastAccess
{
    Allowed,
    Denied,
    Unavailable,
}

public sealed record ToastDetails(
    uint Id,
    string AppUserModelId,
    string DisplayName,
    IReadOnlyList<string> Texts,
    DateTimeOffset CreatedAt);

public interface IToastSignal : IDisposable
{
    bool TryStart(Action onSignal);
}

public interface IToastStore
{
    Task<ToastAccess> RequestAccessAsync(CancellationToken cancellationToken);

    Task<IReadOnlyList<uint>> ListIdsAsync(CancellationToken cancellationToken);

    ToastDetails? GetDetails(uint id);
}
