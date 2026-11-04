using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.UI.Shell;
using Windows.Win32.UI.WindowsAndMessaging;

namespace IslandBridge.Shell;

internal sealed record TrayMenuItem(uint Id, string Text, bool Enabled = true, bool Checked = false, IReadOnlyList<TrayMenuItem>? Children = null)
{
    public static TrayMenuItem Separator { get; } = new(0, string.Empty);

    public bool IsSeparator => Id == 0 && Text.Length == 0;
}

internal sealed unsafe class TrayIcon : IDisposable
{
    private const uint CallbackMessage = PInvoke.WM_APP + 1;
    private const uint InvokeMessage = PInvoke.WM_APP + 2;
    private const uint IconId = 1;
    private const uint KeySelectNotification = PInvoke.NIN_SELECT | 1;
    private const string WindowClassName = "IslandBridge.Tray";

    private static TrayIcon? s_current;

    private readonly ConcurrentQueue<Action> _invocations = new();
    private readonly uint _taskbarCreatedMessage;
    private readonly HWND _window;
    private readonly HICON _icon;
    private string _tooltip;
    private bool _added;
    private bool _disposed;

    public TrayIcon(string iconPath, string tooltip)
    {
        if (s_current is not null)
        {
            throw new InvalidOperationException("Only one tray icon is supported.");
        }

        s_current = this;
        _tooltip = tooltip;
        _taskbarCreatedMessage = PInvoke.RegisterWindowMessage("TaskbarCreated");
        var instance = new HINSTANCE(PInvoke.GetModuleHandle((char*)null).Value);

        fixed (char* className = WindowClassName)
        {
            var windowClass = new WNDCLASSEXW
            {
                cbSize = (uint)sizeof(WNDCLASSEXW),
                lpfnWndProc = &WindowProcedure,
                hInstance = instance,
                lpszClassName = className,
            };

            if (PInvoke.RegisterClassEx(&windowClass) == 0)
            {
                throw new InvalidOperationException($"RegisterClassEx failed: {Marshal.GetLastPInvokeError()}");
            }

            fixed (char* title = "IslandBridge")
            {
                _window = PInvoke.CreateWindowEx(0, new PCWSTR(className), new PCWSTR(title), WINDOW_STYLE.WS_OVERLAPPED, 0, 0, 0, 0, HWND.Null, HMENU.Null, instance, null);
            }
        }

        if (_window.IsNull)
        {
            throw new InvalidOperationException($"CreateWindowEx failed: {Marshal.GetLastPInvokeError()}");
        }

        _icon = LoadIcon(iconPath);
        Add();
    }

    public event EventHandler<TrayPointerEventArgs>? Activated;

    public event EventHandler<TrayPointerEventArgs>? MenuRequested;

    public bool IsAdded => _added;

    public void Post(Action action)
    {
        ArgumentNullException.ThrowIfNull(action);
        _invocations.Enqueue(action);
        PInvoke.PostMessage(_window, InvokeMessage, default, default);
    }

    public void SetTooltip(string tooltip)
    {
        _tooltip = tooltip;

        if (!_added)
        {
            return;
        }

        var data = CreateData(NOTIFY_ICON_DATA_FLAGS.NIF_TIP | NOTIFY_ICON_DATA_FLAGS.NIF_SHOWTIP);
        PInvoke.Shell_NotifyIcon(NOTIFY_ICON_MESSAGE.NIM_MODIFY, &data);
    }

    public uint ShowMenu(IReadOnlyList<TrayMenuItem> items, int x, int y)
    {
        var menu = BuildMenu(items);

        try
        {
            PInvoke.SetForegroundWindow(_window);
            var command = PInvoke.TrackPopupMenuEx(
                menu,
                (uint)(TRACK_POPUP_MENU_FLAGS.TPM_RETURNCMD | TRACK_POPUP_MENU_FLAGS.TPM_RIGHTBUTTON | TRACK_POPUP_MENU_FLAGS.TPM_BOTTOMALIGN),
                x,
                y,
                _window,
                null);
            PInvoke.PostMessage(_window, PInvoke.WM_NULL, default, default);
            return (uint)command.Value;
        }
        finally
        {
            PInvoke.DestroyMenu(menu);
        }
    }

    public static void RunMessageLoop()
    {
        MSG message;

        while (PInvoke.GetMessage(&message, HWND.Null, 0, 0).Value > 0)
        {
            PInvoke.TranslateMessage(&message);
            PInvoke.DispatchMessage(&message);
        }
    }

    public static void Quit() => PInvoke.PostQuitMessage(0);

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;

        if (_added)
        {
            var data = CreateData(0);
            PInvoke.Shell_NotifyIcon(NOTIFY_ICON_MESSAGE.NIM_DELETE, &data);
            _added = false;
        }

        if (!_icon.IsNull)
        {
            PInvoke.DestroyIcon(_icon);
        }

        PInvoke.DestroyWindow(_window);
        s_current = null;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvStdcall)])]
    private static LRESULT WindowProcedure(HWND window, uint message, WPARAM wParam, LPARAM lParam)
    {
        if (s_current is { } tray && tray._window == window)
        {
            try
            {
                if (tray.Handle(message, wParam, lParam))
                {
                    return new LRESULT(0);
                }
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                Environment.FailFast("Unhandled exception in the tray window procedure.", exception);
            }
        }

        return PInvoke.DefWindowProc(window, message, wParam, lParam);
    }

    private static HICON LoadIcon(string path)
    {
        var size = PInvoke.GetSystemMetrics(SYSTEM_METRICS_INDEX.SM_CXSMICON);

        fixed (char* file = path)
        {
            var handle = PInvoke.LoadImage(HINSTANCE.Null, file, GDI_IMAGE_TYPE.IMAGE_ICON, size, size, IMAGE_FLAGS.LR_LOADFROMFILE);
            return new HICON(handle.Value);
        }
    }

    private static void CopyText(Span<char> destination, string text)
    {
        destination.Clear();
        var length = Math.Min(text.Length, destination.Length - 1);
        text.AsSpan(0, length).CopyTo(destination);
    }

    private static HMENU BuildMenu(IReadOnlyList<TrayMenuItem> items)
    {
        var menu = PInvoke.CreatePopupMenu();

        foreach (var item in items)
        {
            if (item.IsSeparator)
            {
                PInvoke.AppendMenu(menu, MENU_ITEM_FLAGS.MF_SEPARATOR, 0, (char*)null);
                continue;
            }

            var flags = MENU_ITEM_FLAGS.MF_STRING;

            if (!item.Enabled)
            {
                flags |= MENU_ITEM_FLAGS.MF_GRAYED;
            }

            if (item.Checked)
            {
                flags |= MENU_ITEM_FLAGS.MF_CHECKED;
            }

            nuint id = item.Id;

            if (item.Children is { Count: > 0 } children)
            {
                flags |= MENU_ITEM_FLAGS.MF_POPUP;
                id = (nuint)(nint)BuildMenu(children).Value;
            }

            fixed (char* text = item.Text)
            {
                PInvoke.AppendMenu(menu, flags, id, text);
            }
        }

        return menu;
    }

    private bool Handle(uint message, WPARAM wParam, LPARAM lParam)
    {
        if (message == CallbackMessage)
        {
            var notification = (uint)(lParam.Value & 0xFFFF);

            if (notification is PInvoke.NIN_SELECT or KeySelectNotification)
            {
                Activated?.Invoke(this, new TrayPointerEventArgs(
                    (short)(wParam.Value & 0xFFFF),
                    (short)((wParam.Value >> 16) & 0xFFFF)));
            }
            else if (notification == PInvoke.WM_CONTEXTMENU)
            {
                MenuRequested?.Invoke(this, new TrayPointerEventArgs(
                    (short)(wParam.Value & 0xFFFF),
                    (short)((wParam.Value >> 16) & 0xFFFF)));
            }

            return true;
        }

        if (message == InvokeMessage)
        {
            while (_invocations.TryDequeue(out var action))
            {
                action();
            }

            return true;
        }

        if (message == _taskbarCreatedMessage)
        {
            _added = false;
            Add();
            return true;
        }

        return false;
    }

    private void Add()
    {
        var data = CreateData(NOTIFY_ICON_DATA_FLAGS.NIF_MESSAGE | NOTIFY_ICON_DATA_FLAGS.NIF_ICON | NOTIFY_ICON_DATA_FLAGS.NIF_TIP | NOTIFY_ICON_DATA_FLAGS.NIF_SHOWTIP);
        _added = PInvoke.Shell_NotifyIcon(NOTIFY_ICON_MESSAGE.NIM_ADD, &data);

        if (_added)
        {
            data.Anonymous.uVersion = PInvoke.NOTIFYICON_VERSION_4;
            PInvoke.Shell_NotifyIcon(NOTIFY_ICON_MESSAGE.NIM_SETVERSION, &data);
        }
    }

    private NOTIFYICONDATAW CreateData(NOTIFY_ICON_DATA_FLAGS flags)
    {
        var data = new NOTIFYICONDATAW
        {
            cbSize = (uint)sizeof(NOTIFYICONDATAW),
            hWnd = _window,
            uID = IconId,
            uFlags = flags,
            uCallbackMessage = CallbackMessage,
            hIcon = _icon,
        };
        CopyText(data.szTip.AsSpan(), _tooltip);
        return data;
    }
}

internal sealed class TrayPointerEventArgs(int x, int y) : EventArgs
{
    public int X { get; } = x;

    public int Y { get; } = y;
}
