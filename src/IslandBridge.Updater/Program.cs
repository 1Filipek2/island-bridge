using System.Diagnostics;
using System.Security.Cryptography;
using System.Security.Principal;
using IslandBridge.Core;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Updates;

namespace IslandBridge.Updater;

// The only IslandBridge process with network access. IslandBridge starts it with "check" or
// "install --wait-pid <pid>" and reads one result line from stdout (UpdaterReport).
internal static class Program
{
    private const string MutexName = @"Local\IslandBridge.Updater";
    private static readonly TimeSpan CheckTimeout = TimeSpan.FromMinutes(1);
    private static readonly TimeSpan DownloadTimeout = TimeSpan.FromMinutes(15);
    private static readonly TimeSpan ParentExitTimeout = TimeSpan.FromSeconds(30);

    private static int Main(string[] args)
    {
        var command = UpdaterCommand.Parse(args);

        if (command.Verb == UpdaterVerb.Invalid)
        {
            return Report(UpdaterReport.Failed(UpdateFailure.Usage));
        }

        using var mutex = new Mutex(true, MutexName, out var owned);

        if (!owned)
        {
            return Report(UpdaterReport.Failed(UpdateFailure.Busy));
        }

        var paths = AppPaths.Default;
        using var log = new FileLog(paths.LogDirectory, "updater");

        try
        {
            if (IsElevated())
            {
                log.Warn("refused: elevated");
                return Report(UpdaterReport.Failed(UpdateFailure.Elevated));
            }

            if (!UpdateSignature.HasTrustedKeys)
            {
                log.Warn("refused: no trusted signing key in this build");
                return Report(UpdaterReport.Failed(UpdateFailure.NoTrustedKey));
            }

            return command.Verb == UpdaterVerb.Check
                ? Report(CheckAsync(log).GetAwaiter().GetResult())
                : InstallAsync(command.ParentProcessId, paths, log).GetAwaiter().GetResult();
        }
        catch (UpdateException exception)
        {
            log.Error($"{exception.Failure}: {exception.Message}");
            return Report(UpdaterReport.Failed(exception.Failure));
        }
        catch (OperationCanceledException)
        {
            log.Error("timed out");
            return Report(UpdaterReport.Failed(UpdateFailure.Network));
        }
        catch (IOException exception)
        {
            log.Error($"io failure: {exception.GetType().Name} 0x{exception.HResult:X8}");
            return Report(UpdaterReport.Failed(UpdateFailure.Download));
        }
        catch (UnauthorizedAccessException exception)
        {
            log.Error($"access failure: {exception.GetType().Name}");
            return Report(UpdaterReport.Failed(UpdateFailure.Download));
        }
        finally
        {
            mutex.ReleaseMutex();
        }
    }

    private static async Task<UpdaterReport> CheckAsync(ILog log)
    {
        using var timeout = new CancellationTokenSource(CheckTimeout);
        using var client = new ReleaseClient();
        var manifest = await FetchManifestAsync(client, timeout.Token).ConfigureAwait(false);
        var report = manifest.Version > UpdateVersion.Current ? UpdaterReport.Available(manifest.Version) : UpdaterReport.Current(UpdateVersion.Current);
        log.Info($"check: installed {UpdateVersion.Current}, latest {manifest.Version}");
        return report;
    }

    private static async Task<int> InstallAsync(int parentProcessId, AppPaths paths, ILog log)
    {
        // Taken now, while the parent is surely alive, so a recycled pid cannot be waited on later.
        using var parent = TryGetProcess(parentProcessId);

        if (!InstallLocation.IsInstalledCopy(AppContext.BaseDirectory))
        {
            log.Warn("install refused: not an installed copy");
            return Report(UpdaterReport.Failed(UpdateFailure.NotInstalled));
        }

        using var timeout = new CancellationTokenSource(DownloadTimeout);
        using var client = new ReleaseClient();
        var manifest = await FetchManifestAsync(client, timeout.Token).ConfigureAwait(false);

        if (manifest.Version <= UpdateVersion.Current)
        {
            log.Info($"install: already at {UpdateVersion.Current}");
            return Report(UpdaterReport.Current(UpdateVersion.Current));
        }

        var directory = PrepareDirectory(Path.Combine(paths.DataDirectory, "updates"));
        var setupPath = Path.Combine(directory, manifest.FileName);
        var partialPath = setupPath + ".partial";

        await using (var partial = new FileStream(partialPath, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1, FileOptions.Asynchronous | FileOptions.SequentialScan))
        {
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            await client.DownloadAsync(UpdateEndpoints.Setup(manifest), partial, manifest.Size, hash, timeout.Token).ConfigureAwait(false);
            await partial.FlushAsync(timeout.Token).ConfigureAwait(false);

            if (!CryptographicOperations.FixedTimeEquals(hash.GetHashAndReset(), manifest.Sha256))
            {
                throw new UpdateException(UpdateFailure.Hash, "downloaded setup does not match the signed hash");
            }
        }

        File.Move(partialPath, setupPath, overwrite: true);

        // Held open without write or delete sharing from the final check until the setup process
        // exists, so the verified bytes are the ones that run.
        using var setup = new FileStream(setupPath, FileMode.Open, FileAccess.Read, FileShare.Read);

        if (setup.Length != manifest.Size || !CryptographicOperations.FixedTimeEquals(SHA256.HashData(setup), manifest.Sha256))
        {
            throw new UpdateException(UpdateFailure.Hash, "setup changed after download");
        }

        log.Info($"install: {manifest.Version} verified, waiting for IslandBridge to exit");
        Report(UpdaterReport.Ready(manifest.Version));

        if (parent is not null && !parent.WaitForExit(ParentExitTimeout))
        {
            log.Warn("install: IslandBridge did not exit in time, setup will stop it");
        }

        try
        {
            using var process = Process.Start(new ProcessStartInfo(setupPath)
            {
                UseShellExecute = false,
                WorkingDirectory = directory,
                ArgumentList = { "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/NOCANCEL", "/SP-", "/relaunch=1" },
            });

            log.Info(process is null ? "install: setup did not start" : "install: setup started");

            if (process is null)
            {
                RestartIslandBridge(log);
                return 1;
            }
        }
        catch (System.ComponentModel.Win32Exception exception)
        {
            log.Error($"install: setup failed to start 0x{exception.NativeErrorCode:X8}");
            RestartIslandBridge(log);
            return 1;
        }

        return 0;
    }

    private static async Task<UpdateManifest> FetchManifestAsync(ReleaseClient client, CancellationToken cancellationToken)
    {
        var manifestBytes = await client.GetSmallAsync(UpdateEndpoints.Manifest, UpdateManifest.MaxBytes, cancellationToken).ConfigureAwait(false);
        var signatureBytes = await client.GetSmallAsync(UpdateEndpoints.Signature, UpdateSignature.MaxFileBytes, cancellationToken).ConfigureAwait(false);

        // The signature is checked before the manifest is parsed, so the parser only sees signed bytes.
        if (!UpdateSignature.Verify(manifestBytes, signatureBytes))
        {
            throw new UpdateException(UpdateFailure.Signature, "manifest signature is not valid");
        }

        return UpdateManifest.TryParse(manifestBytes, out var manifest) && manifest is not null
            ? manifest
            : throw new UpdateException(UpdateFailure.Manifest, "signed manifest is malformed");
    }

    private static string PrepareDirectory(string directory)
    {
        var info = new DirectoryInfo(directory);

        if (info.Exists && info.Attributes.HasFlag(FileAttributes.ReparsePoint))
        {
            throw new UpdateException(UpdateFailure.Download, "updates folder is a reparse point");
        }

        info.Create();

        foreach (var file in info.EnumerateFiles())
        {
            file.Delete();
        }

        return info.FullName;
    }

    private static void RestartIslandBridge(ILog log)
    {
        try
        {
            using var process = Process.Start(new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory, "IslandBridge.exe"))
            {
                UseShellExecute = false,
                WorkingDirectory = AppContext.BaseDirectory,
            });
        }
        catch (System.ComponentModel.Win32Exception exception)
        {
            log.Error($"restart failed 0x{exception.NativeErrorCode:X8}");
        }
    }

    private static Process? TryGetProcess(int processId)
    {
        Process? process = null;

        try
        {
            process = Process.GetProcessById(processId);
            _ = process.SafeHandle;
            return process;
        }
        catch (Exception exception) when (exception is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            process?.Dispose();
            return null;
        }
    }

    private static bool IsElevated()
    {
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    private static int Report(UpdaterReport report)
    {
        Console.Out.WriteLine(report.ToString());
        Console.Out.Flush();
        return report.Outcome == UpdaterOutcome.Failed ? 1 : 0;
    }
}
