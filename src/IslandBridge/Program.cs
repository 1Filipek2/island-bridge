using System.Diagnostics;
using IslandBridge.Core;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Startup;
using IslandBridge.Shell;
using Windows.Graphics;

namespace IslandBridge;

internal static class Program
{
    private const uint OpenCommand = 1;
    private const uint TestCommand = 2;
    private const uint Pause15Command = 3;
    private const uint Pause60Command = 4;
    private const uint Pause480Command = 5;
    private const uint PauseIndefinitelyCommand = 6;
    private const uint ResumeCommand = 7;
    private const uint ExitCommand = 8;
    private const uint StatusCommand = 9;

    [STAThread]
    private static int Main(string[] args)
    {
        var started = Stopwatch.GetTimestamp();
        var arguments = StartupArguments.Parse(args);
        using var instance = SingleInstance.TryAcquire();

        if (instance is null)
        {
            if (!arguments.Autostart)
            {
                SingleInstance.SignalRunningInstance();
            }

            return 0;
        }

        WinRT.ComWrappersSupport.InitializeComWrappers();
        var paths = AppPaths.Default;
        using var log = new FileLog(paths.LogDirectory);
        log.Info($"start {(arguments.Autostart ? "autostart" : "manual")}");

        var host = new AppHost(paths, log);
        host.CompleteFirstRun();
        var ui = new UiHost(host);

        using (var tray = new TrayIcon(host.IconPath, Tooltip(host)))
        {
            log.Info($"tray ready in {Stopwatch.GetElapsedTime(started).TotalMilliseconds:F0} ms");

            tray.Activated += (_, pointer) => ui.Show(new PointInt32(pointer.X, pointer.Y), toggle: true);
            tray.MenuRequested += (_, menuArgs) => HandleMenu(tray, host, ui, menuArgs.X, menuArgs.Y);
            host.StatusChanged += (_, _) => tray.Post(() => tray.SetTooltip(Tooltip(host)));
            host.ConfigChanged += (_, _) => tray.Post(() => tray.SetTooltip(Tooltip(host)));
            host.ShowWindowRequested += (_, _) => tray.Post(() => ui.Show());
            host.ExitRequested += (_, _) => tray.Post(TrayIcon.Quit);
            instance.OnShowRequested(host.RequestShowWindow);

            if (arguments.ShouldShowWindow(host.Config.Startup))
            {
                ui.Show();
            }

            host.SyncWindowsToasts();
            host.StartBluetooth(arguments.BluetoothDelay(host.Config.Startup));
            TrayIcon.RunMessageLoop();

            log.Info("exit requested");
            ui.Shutdown();
        }

        host.DisposeAsync().AsTask().GetAwaiter().GetResult();
        log.Info("exit");
        return 0;
    }

    private static string Tooltip(AppHost host) => $"{host.Localizer["App_Name"]}: {StatusPresenter.Describe(host)}";

    private static void HandleMenu(TrayIcon tray, AppHost host, UiHost ui, int x, int y)
    {
        var localizer = host.Localizer;
        var paused = host.PausedUntil is not null;
        IReadOnlyList<TrayMenuItem> pauseItems =
        [
            new(Pause15Command, localizer["Tray_Pause15"]),
            new(Pause60Command, localizer["Tray_Pause60"]),
            new(Pause480Command, localizer["Tray_Pause480"]),
            new(PauseIndefinitelyCommand, localizer["Tray_PauseUntilResumed"]),
        ];

        IReadOnlyList<TrayMenuItem> items =
        [
            new(StatusCommand, StatusPresenter.Describe(host), Enabled: false),
            TrayMenuItem.Separator,
            new(OpenCommand, localizer["Tray_Open"]),
            new(TestCommand, localizer["Tray_Test"]),
            paused ? new(ResumeCommand, localizer["Tray_Resume"]) : new(100, localizer["Tray_Pause"], Children: pauseItems),
            TrayMenuItem.Separator,
            new(ExitCommand, localizer["Tray_Exit"]),
        ];

        switch (tray.ShowMenu(items, x, y))
        {
            case OpenCommand:
                ui.Show(new PointInt32(x, y));
                break;
            case TestCommand:
                host.SendTestNotification();
                break;
            case Pause15Command:
                host.Pause(PauseOption.Minutes15);
                break;
            case Pause60Command:
                host.Pause(PauseOption.Hour1);
                break;
            case Pause480Command:
                host.Pause(PauseOption.Hours8);
                break;
            case PauseIndefinitelyCommand:
                host.Pause(PauseOption.UntilResumed);
                break;
            case ResumeCommand:
                host.Resume();
                break;
            case ExitCommand:
                TrayIcon.Quit();
                break;
        }
    }
}
