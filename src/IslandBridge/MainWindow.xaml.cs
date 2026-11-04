using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using IslandBridge.Controls;
using IslandBridge.Core.Ancs;
using IslandBridge.Core.Configuration;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Notifications;
using IslandBridge.Core.Startup;
using IslandBridge.Core.Toasts;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics;
using Windows.System;
using Windows.UI;
using Windows.Win32;
using Windows.Win32.Foundation;

namespace IslandBridge;

public sealed partial class MainWindow : Window
{
    private const int WidthDip = 380;
    private const int MaxHeightDip = 720;
    private const int EdgeMarginDip = 12;
    private const int RecentShown = 5;

    private static readonly Color Green = Color.FromArgb(255, 0x3F, 0xB9, 0x50);
    private static readonly Color Amber = Color.FromArgb(255, 0xE8, 0xA3, 0x17);
    private static readonly Color Red = Color.FromArgb(255, 0xE5, 0x48, 0x4D);
    private static readonly Color Gray = Color.FromArgb(255, 0x8A, 0x8A, 0x8A);

    private readonly AppHost _host;
    private readonly long _requestedAt;
    private readonly OverlappedPresenter _presenter = OverlappedPresenter.Create();
    private bool _ready;
    private bool _activatedOnce;
    private bool _closed;
    private bool _minimized;
    private bool _minimizeApplied;

    internal MainWindow(AppHost host, PointInt32? anchor, long requestedAt)
    {
        _host = host;
        _requestedAt = requestedAt;
        InitializeComponent();

        ConfigureChrome();
        PlaceNear(anchor);

        foreach (var toggle in new[] { IgnoreSilentSwitch, PreviewsSwitch, WindowsToastsSwitch, AutostartSwitch, MinimizedSwitch })
        {
            MakeCompact(toggle);
        }

        ApplyTheme();
        Logo.Source = new BitmapImage(new Uri(host.IconPath));
        ApplyStrings();
        LoadValues();
        UpdateStatus();
        UpdateRecent();

        PauseSegments.SegmentSelected += OnPauseSelected;
        DelaySegments.SegmentSelected += OnDelaySelected;
        LanguageSegments.SegmentSelected += OnLanguageSelected;
        ThemeSegments.SegmentSelected += OnThemeSelected;
        CloseActionSegments.SegmentSelected += OnCloseActionSelected;
        Activated += OnActivated;
        Closed += OnClosed;
        host.StatusChanged += OnHostStatusChanged;
        host.NotificationsChanged += OnHostNotificationsChanged;
        host.ConfigChanged += OnHostConfigChanged;
        host.Localizer.LanguageChanged += OnLanguageChanged;
        _ready = true;
    }

    internal bool IsLightDismissed { get; private set; }

    internal bool IsMinimized => _minimized || _presenter.State == OverlappedPresenterState.Minimized;

    internal void BringToFront()
    {
        if (_presenter.State == OverlappedPresenterState.Minimized)
        {
            _presenter.Restore();
        }

        Activate();
        PInvoke.SetForegroundWindow(WindowHandle);
    }

    private HWND WindowHandle => new(Win32Interop.GetWindowFromWindowId(AppWindow.Id));

    private double Scale => PInvoke.GetDpiForWindow(WindowHandle) / 96.0;

    private void ConfigureChrome()
    {
        _presenter.IsResizable = false;
        _presenter.IsMaximizable = false;
        _presenter.IsMinimizable = true;
        _presenter.IsAlwaysOnTop = true;
        _presenter.SetBorderAndTitleBar(hasBorder: true, hasTitleBar: false);
        AppWindow.SetPresenter(_presenter);
        ExtendsContentIntoTitleBar = true;

        AppWindow.IsShownInSwitchers = false;
        AppWindow.SetIcon(_host.IconPath);
        Title = _host.Localizer["App_Name"];
    }

    private void PlaceNear(PointInt32? anchor)
    {
        var point = anchor ?? CursorPosition();
        var area = DisplayArea.GetFromPoint(point, DisplayAreaFallback.Nearest).WorkArea;
        var scale = Scale;
        var margin = (int)(EdgeMarginDip * scale);
        var width = (int)(WidthDip * scale);
        var height = Math.Min((int)(MaxHeightDip * scale), area.Height - (2 * margin));

        var x = Math.Clamp(point.X - (width / 2), area.X + margin, area.X + area.Width - width - margin);
        var y = Math.Clamp(point.Y - height - margin, area.Y + margin, area.Y + area.Height - height - margin);
        AppWindow.MoveAndResize(new RectInt32(x, y, width, height));
    }

    private static unsafe PointInt32 CursorPosition()
    {
        System.Drawing.Point point;
        PInvoke.GetCursorPos(&point);
        return new PointInt32(point.X, point.Y);
    }

    private void ApplyTheme()
    {
        Root.RequestedTheme = _host.Config.Appearance.Theme switch
        {
            AppTheme.Light => ElementTheme.Light,
            AppTheme.Dark => ElementTheme.Dark,
            _ => ElementTheme.Default,
        };
    }

    private void ApplyStrings()
    {
        var localizer = _host.Localizer;
        AppTitle.Text = localizer["App_Name"];
        NotificationsHeader.Text = localizer["Section_Notifications"];
        IgnoreSilentLabel.Text = localizer["Setting_IgnoreSilent"];
        PreviewsLabel.Text = localizer["Setting_Previews"];
        WindowsToastsLabel.Text = localizer["Setting_WindowsToasts"];
        RecentHeader.Text = localizer["Section_Recent"];
        TestButton.Content = localizer["Test_Button"];
        RecentEmpty.Text = localizer["Recent_Empty"];
        DeviceHeader.Text = localizer["Section_Device"];
        FindButton.Content = localizer["Device_Find"];
        ReconnectButton.Content = localizer["Device_Reconnect"];
        ForgetButton.Content = localizer["Device_Forget"];
        DeviceHelp.Text = localizer["Device_Help"];
        StartupHeader.Text = localizer["Section_Startup"];
        AutostartLabel.Text = localizer["Setting_Autostart"];
        AutostartWarning.Text = localizer["Setting_AutostartDisabledByUser"];
        MinimizedLabel.Text = localizer["Setting_StartMinimized"];
        CloseActionLabel.Text = localizer["Setting_CloseAction"];
        ToolTipService.SetToolTip(MinimizeButton, localizer["Caption_Minimize"]);
        ToolTipService.SetToolTip(CloseButton, localizer["Caption_Close"]);
        DelayLabel.Text = localizer["Setting_Delay"];
        AppearanceHeader.Text = localizer["Section_Appearance"];
        LanguageLabel.Text = localizer["Setting_Language"];
        ThemeLabel.Text = localizer["Setting_Theme"];
        FiltersExpander.Header = localizer["Section_Filters"];
        CategoriesHeader.Text = localizer["Filters_Categories"];
        AppsHeader.Text = localizer["Filters_Apps"];
        AppsEmpty.Text = localizer["Filters_AppsEmpty"];
        LogsButton.Content = localizer["Footer_Logs"];
        ExitButton.Content = localizer["Footer_Exit"];
        ToolTipService.SetToolTip(VersionText, localizer["Footer_Privacy"]);
        VersionText.Text = $"{localizer["App_Name"]} {Version()}";

        PauseSegments.SetItems([localizer["Pause_Active"], localizer["Pause_15"], localizer["Pause_60"], localizer["Pause_480"], localizer["Pause_Off"]]);
        DelaySegments.SetItems([.. AppConfig.AllowedStartupDelays.Select(delay => delay == 0 ? localizer["Setting_DelayNone"] : localizer.Format("Setting_DelaySeconds", delay))]);
        LanguageSegments.SetItems([localizer["Choice_Auto"], localizer["Lang_English"], localizer["Lang_Slovak"]]);
        ThemeSegments.SetItems([localizer["Choice_Auto"], localizer["Theme_Light"], localizer["Theme_Dark"]]);
        CloseActionSegments.SetItems([localizer["CloseAction_Tray"], localizer["CloseAction_Exit"]]);
        BuildFilters();
    }

    private void LoadValues()
    {
        var wasReady = _ready;
        _ready = false;
        var config = _host.Config;
        IgnoreSilentSwitch.IsOn = config.Notifications.IgnoreSilent;
        PreviewsSwitch.IsOn = config.Notifications.ShowPreviews;
        WindowsToastsSwitch.IsOn = config.Notifications.WindowsToasts;
        MinimizedSwitch.IsOn = config.Startup.StartMinimized;
        PauseSegments.SelectedIndex = (int)_host.CurrentPause;
        DelaySegments.SelectedIndex = Math.Max(0, AppConfig.AllowedStartupDelays.ToList().IndexOf(config.Startup.DelaySeconds));
        LanguageSegments.SelectedIndex = (int)config.Appearance.Language;
        ThemeSegments.SelectedIndex = (int)config.Appearance.Theme;
        CloseActionSegments.SelectedIndex = (int)config.Startup.CloseAction;
        RefreshAutostart();
        _ready = wasReady;
    }

    private void RefreshAutostart()
    {
        var wasReady = _ready;
        _ready = false;
        var state = _host.Autostart.GetState();
        AutostartSwitch.IsOn = state == AutostartState.Enabled;
        AutostartWarning.Visibility = state == AutostartState.DisabledByUser ? Visibility.Visible : Visibility.Collapsed;
        _ready = wasReady;
    }

    private void UpdateStatus()
    {
        StatusText.Text = StatusPresenter.Describe(_host);
        StatusDot.Fill = new SolidColorBrush(_host.Status switch
        {
            HostStatus.Ready when _host.PausedUntil is null => Green,
            HostStatus.Ready or HostStatus.Connecting or HostStatus.Delayed or HostStatus.WaitingForDevice => Amber,
            HostStatus.PermissionRequired or HostStatus.Retrying => Red,
            _ => Gray,
        });

        var (kind, text) = StatusPresenter.Hint(_host);
        HintBar.IsOpen = kind != HintKind.None;
        HintBar.Severity = kind == HintKind.Warning ? InfoBarSeverity.Warning : InfoBarSeverity.Informational;
        HintBar.Message = text;

        var hasDevice = _host.Config.Device.DeviceId is not null;
        DeviceName.Text = hasDevice ? _host.Config.Device.Name ?? _host.Localizer["Device_YourIphone"] : _host.Localizer["Device_None"];
        ReconnectButton.IsEnabled = hasDevice;
        ForgetButton.IsEnabled = hasDevice;
        DeviceHelp.Visibility = hasDevice ? Visibility.Collapsed : Visibility.Visible;

        var toastWarning = _host.Config.Notifications.WindowsToasts ? _host.ToastState switch
        {
            ToastWatcherState.AccessDenied => _host.Localizer["Setting_WindowsToastsDenied"],
            ToastWatcherState.Unavailable => _host.Localizer["Setting_WindowsToastsUnavailable"],
            _ => null,
        } : null;
        WindowsToastsWarning.Text = toastWarning ?? string.Empty;
        WindowsToastsWarning.Visibility = toastWarning is null ? Visibility.Collapsed : Visibility.Visible;

        var wasReady = _ready;
        _ready = false;
        PauseSegments.SelectedIndex = (int)_host.CurrentPause;
        _ready = wasReady;
    }

    private void UpdateRecent()
    {
        var recent = _host.Recent;
        var localizer = _host.Localizer;
        var showText = _host.Config.Notifications.ShowPreviews;
        RecentEmpty.Visibility = recent.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        RecentList.Children.Clear();

        foreach (var item in recent.Take(RecentShown))
        {
            var meta = item.ReceivedAt.ToString("HH:mm", CultureInfo.InvariantCulture);

            if (item.Decision != FilterDecision.Deliver)
            {
                meta = $"{localizer[$"Filter_{item.Decision}"]} · {meta}";
            }

            var body = showText
                ? item.Title.Length > 0 && item.Message.Length > 0 ? $"{item.Title}: {item.Message}" : item.Title + item.Message
                : item.Origin == NotificationOrigin.Windows ? localizer["Origin_Windows"] : localizer[$"Category_{item.Category}"];
            RecentList.Children.Add(new Card(item.AppName, meta, body) { Opacity = item.Decision == FilterDecision.Deliver ? 1 : 0.55 });
        }
    }

    private void BuildFilters()
    {
        var localizer = _host.Localizer;
        var config = _host.Config.Notifications;
        CategoryList.Children.Clear();

        foreach (var category in Enum.GetValues<AncsCategory>())
        {
            var box = new CheckBox
            {
                Content = localizer[$"Category_{category}"],
                IsChecked = !config.MutedCategories.Contains(category),
                FontSize = 12,
                MinWidth = 0,
            };
            box.Click += (_, _) => SetCategoryMuted(category, box.IsChecked != true);
            CategoryList.Children.Add(box);
        }

        var apps = _host.Config.AppNames.OrderBy(entry => entry.Value, StringComparer.OrdinalIgnoreCase).ToList();
        AppsEmpty.Visibility = apps.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        AppList.Children.Clear();

        foreach (var (identifier, name) in apps)
        {
            var row = new Grid { ColumnSpacing = 8 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.Children.Add(new TextBlock { Text = name, FontSize = 13, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis });
            var toggle = new ToggleSwitch { IsOn = !config.MutedApps.Contains(identifier, StringComparer.Ordinal) };
            MakeCompact(toggle);
            ToolTipService.SetToolTip(toggle, identifier);
            toggle.Toggled += (_, _) => SetAppMuted(identifier, !toggle.IsOn);
            Grid.SetColumn(toggle, 1);
            row.Children.Add(toggle);
            AppList.Children.Add(row);
        }
    }

    private static void MakeCompact(ToggleSwitch toggle)
    {
        toggle.OnContent = string.Empty;
        toggle.OffContent = string.Empty;
        toggle.MinWidth = 0;
        toggle.HorizontalAlignment = HorizontalAlignment.Right;
        toggle.Margin = new Thickness(0, -6, -12, -6);
    }

    private static string Version()
    {
        var version = typeof(MainWindow).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "0.0.0";
        var plus = version.IndexOf('+', StringComparison.Ordinal);
        return plus > 0 ? version[..plus] : version;
    }

    private void OnActivated(object sender, WindowActivatedEventArgs args)
    {
        if (args.WindowActivationState == WindowActivationState.Deactivated)
        {
            if (_activatedOnce && !_minimized)
            {
                IsLightDismissed = true;
                Close();
            }

            return;
        }

        LeaveMinimizedIfRestored();

        if (!_activatedOnce)
        {
            _activatedOnce = true;
            _host.Log.Info($"window shown in {Stopwatch.GetElapsedTime(_requestedAt).TotalMilliseconds:F0} ms");
        }
    }

    private void OnMinimizeClick(object sender, RoutedEventArgs args)
    {
        _minimized = true;
        AppWindow.IsShownInSwitchers = true;
        DispatcherQueue.TryEnqueue(() =>
        {
            if (!_closed && _minimized)
            {
                _presenter.Minimize();
                _minimizeApplied = true;
            }
        });
    }

    private void LeaveMinimizedIfRestored()
    {
        if (!_minimizeApplied || _presenter.State == OverlappedPresenterState.Minimized)
        {
            return;
        }

        _minimizeApplied = false;
        _minimized = false;
        AppWindow.IsShownInSwitchers = false;
    }

    private void OnCloseClick(object sender, RoutedEventArgs args)
    {
        if (_host.Config.Startup.CloseAction == CloseAction.Exit)
        {
            _host.RequestExit();
            return;
        }

        Close();
    }

    private void OnCloseActionSelected(object? sender, SegmentSelectedEventArgs args)
    {
        if (_ready)
        {
            _host.UpdateConfig(config => config.Startup.CloseAction = (CloseAction)args.Index);
        }
    }

    private void OnRootKeyDown(object sender, KeyRoutedEventArgs args)
    {
        if (args.Key == VirtualKey.Escape)
        {
            args.Handled = true;
            Close();
        }
    }

    private void OnPauseSelected(object? sender, SegmentSelectedEventArgs args)
    {
        if (_ready)
        {
            _host.Pause((PauseOption)args.Index);
        }
    }

    private void OnDelaySelected(object? sender, SegmentSelectedEventArgs args)
    {
        if (_ready)
        {
            var delay = AppConfig.AllowedStartupDelays[args.Index];
            _host.UpdateConfig(config => config.Startup.DelaySeconds = delay);
        }
    }

    private void OnLanguageSelected(object? sender, SegmentSelectedEventArgs args)
    {
        if (_ready)
        {
            _host.UpdateConfig(config => config.Appearance.Language = (AppLanguage)args.Index);
        }
    }

    private void OnThemeSelected(object? sender, SegmentSelectedEventArgs args)
    {
        if (_ready)
        {
            _host.UpdateConfig(config => config.Appearance.Theme = (AppTheme)args.Index);
        }
    }

    private void OnIgnoreSilentToggled(object sender, RoutedEventArgs args)
    {
        if (_ready)
        {
            _host.UpdateConfig(config => config.Notifications.IgnoreSilent = IgnoreSilentSwitch.IsOn);
        }
    }

    private void OnPreviewsToggled(object sender, RoutedEventArgs args)
    {
        if (_ready)
        {
            _host.UpdateConfig(config => config.Notifications.ShowPreviews = PreviewsSwitch.IsOn);
        }
    }

    private void OnWindowsToastsToggled(object sender, RoutedEventArgs args)
    {
        if (_ready)
        {
            _host.UpdateConfig(config => config.Notifications.WindowsToasts = WindowsToastsSwitch.IsOn);
        }
    }

    private void OnMinimizedToggled(object sender, RoutedEventArgs args)
    {
        if (_ready)
        {
            _host.UpdateConfig(config => config.Startup.StartMinimized = MinimizedSwitch.IsOn);
        }
    }

    private void OnAutostartToggled(object sender, RoutedEventArgs args)
    {
        if (!_ready)
        {
            return;
        }

        try
        {
            if (AutostartSwitch.IsOn)
            {
                _host.Autostart.Enable(_host.ExecutablePath);
            }
            else
            {
                _host.Autostart.Disable();
            }

            _host.Log.Info($"autostart {(AutostartSwitch.IsOn ? "enabled" : "disabled")} by user");
        }
        catch (UnauthorizedAccessException exception)
        {
            _host.Log.Error($"autostart change failed: {exception.GetType().Name}");
        }

        RefreshAutostart();
    }

    private void OnTestClick(object sender, RoutedEventArgs args) => _host.SendTestNotification();

    private void OnReconnectClick(object sender, RoutedEventArgs args) => _host.StartBluetooth(TimeSpan.Zero);

    private void OnForgetClick(object sender, RoutedEventArgs args) => _host.ForgetDevice();

    private async void OnFindClick(object sender, RoutedEventArgs args)
    {
        FindButton.IsEnabled = false;
        SearchProgress.Visibility = Visibility.Visible;
        SearchStatus.Visibility = Visibility.Visible;
        SearchStatus.Text = _host.Localizer["Device_Searching"];
        Candidates.Children.Clear();

        try
        {
            var found = await Task.Run(() => AncsDeviceFinder.FindAsync(CancellationToken.None));

            if (_closed)
            {
                return;
            }

            SearchStatus.Text = _host.Localizer["Device_FoundNone"];
            SearchStatus.Visibility = found.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

            foreach (var candidate in found)
            {
                var card = new Card(candidate.Name, string.Empty, string.Empty);
                card.SetAction(_host.Localizer["Device_Use"], () =>
                {
                    _host.UseDevice(candidate);
                    Candidates.Children.Clear();
                });
                Candidates.Children.Add(card);
            }
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _host.Log.Error($"device search failed: {exception.GetType().Name} 0x{exception.HResult:X8}");
            SearchStatus.Text = _host.Localizer["Device_FoundNone"];
        }

        FindButton.IsEnabled = true;
        SearchProgress.Visibility = Visibility.Collapsed;
    }

    private void OnLogsClick(object sender, RoutedEventArgs args)
    {
        var directory = _host.Paths.LogDirectory;
        Directory.CreateDirectory(directory);
        using var process = Process.Start(new ProcessStartInfo("explorer.exe") { ArgumentList = { directory }, UseShellExecute = false });
    }

    private void OnExitClick(object sender, RoutedEventArgs args) => _host.RequestExit();

    private void SetCategoryMuted(AncsCategory category, bool muted) => _host.UpdateConfig(config =>
    {
        config.Notifications.MutedCategories.Remove(category);

        if (muted)
        {
            config.Notifications.MutedCategories.Add(category);
        }
    });

    private void SetAppMuted(string identifier, bool muted) => _host.UpdateConfig(config =>
    {
        config.Notifications.MutedApps.Remove(identifier);

        if (muted)
        {
            config.Notifications.MutedApps.Add(identifier);
        }
    });

    private void OnHostStatusChanged(object? sender, EventArgs args) => DispatcherQueue.TryEnqueue(UpdateStatus);

    private void OnHostNotificationsChanged(object? sender, EventArgs args) => DispatcherQueue.TryEnqueue(UpdateRecent);

    private void OnHostConfigChanged(object? sender, EventArgs args) => DispatcherQueue.TryEnqueue(() =>
    {
        ApplyTheme();
        UpdateRecent();
        UpdateStatus();
    });

    private void OnLanguageChanged(object? sender, EventArgs args) => DispatcherQueue.TryEnqueue(() =>
    {
        _ready = false;
        ApplyStrings();
        LoadValues();
        UpdateStatus();
        UpdateRecent();
        _ready = true;
    });

    private void OnClosed(object sender, WindowEventArgs args)
    {
        _closed = true;
        PauseSegments.SegmentSelected -= OnPauseSelected;
        DelaySegments.SegmentSelected -= OnDelaySelected;
        LanguageSegments.SegmentSelected -= OnLanguageSelected;
        ThemeSegments.SegmentSelected -= OnThemeSelected;
        CloseActionSegments.SegmentSelected -= OnCloseActionSelected;
        Activated -= OnActivated;
        Closed -= OnClosed;
        _host.StatusChanged -= OnHostStatusChanged;
        _host.NotificationsChanged -= OnHostNotificationsChanged;
        _host.ConfigChanged -= OnHostConfigChanged;
        _host.Localizer.LanguageChanged -= OnLanguageChanged;
    }
}
