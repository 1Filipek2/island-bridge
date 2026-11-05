using System.Diagnostics;

namespace IslandBridge.Core.Island;

public sealed class IslandLauncher : IIslandLauncher
{
    public const string InstanceMutexName = @"Local\IslandBridge.Island.Instance";
    public const string StopEventName = @"Local\IslandBridge.Island.Stop";

    private readonly string _path;

    public IslandLauncher(string? path = null)
    {
        _path = path ?? PipeServerVerifier.IslandPath;
    }

    public bool Exists => File.Exists(_path);

    public IIslandProcess Start()
    {
        var info = new ProcessStartInfo(_path)
        {
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(_path) ?? AppContext.BaseDirectory,
        };

        var process = Process.Start(info) ?? throw new InvalidOperationException("island process did not start");
        return new IslandProcess(process);
    }

    public void RequestStop()
    {
        if (EventWaitHandle.TryOpenExisting(StopEventName, out var stop))
        {
            using (stop)
            {
                stop.Set();
            }
        }
    }

    public Task WaitForOtherInstanceAsync(CancellationToken cancellationToken) =>
        Task.Factory.StartNew(() => WaitForInstanceMutex(cancellationToken), cancellationToken, TaskCreationOptions.LongRunning, TaskScheduler.Default);

    private static void WaitForInstanceMutex(CancellationToken cancellationToken)
    {
        if (!Mutex.TryOpenExisting(InstanceMutexName, out var mutex))
        {
            return;
        }

        using (mutex)
        {
            var acquired = false;

            try
            {
                acquired = WaitHandle.WaitAny([mutex, cancellationToken.WaitHandle]) == 0;
            }
            catch (AbandonedMutexException)
            {
                acquired = true;
            }

            if (acquired)
            {
                mutex.ReleaseMutex();
            }
        }

        cancellationToken.ThrowIfCancellationRequested();
    }

    private sealed class IslandProcess(Process process) : IIslandProcess
    {
        public async Task<int> WaitForExitAsync(CancellationToken cancellationToken)
        {
            await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
            return process.ExitCode;
        }

        public void Kill()
        {
            try
            {
                process.Kill();
            }
            catch (InvalidOperationException)
            {
            }
        }

        public void Dispose() => process.Dispose();
    }
}
