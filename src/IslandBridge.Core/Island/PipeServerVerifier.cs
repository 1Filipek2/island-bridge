using System.IO.Pipes;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace IslandBridge.Core.Island;

public static partial class PipeServerVerifier
{
    private const uint ProcessQueryLimitedInformation = 0x1000;

    public const string IslandFileName = "IslandBridge.Island.exe";

    public static string IslandPath { get; } = Path.Combine(AppContext.BaseDirectory, IslandFileName);

    public static bool IsIsland(NamedPipeClientStream pipe)
    {
        ArgumentNullException.ThrowIfNull(pipe);
        var path = ServerImagePath(pipe.SafePipeHandle);
        return path is not null && string.Equals(Path.GetFullPath(path), Path.GetFullPath(IslandPath), StringComparison.OrdinalIgnoreCase);
    }

    public static string? ServerImagePath(SafePipeHandle pipe)
    {
        ArgumentNullException.ThrowIfNull(pipe);

        if (!GetNamedPipeServerProcessId(pipe, out var processId) || processId == 0)
        {
            return null;
        }

        using var process = OpenProcess(ProcessQueryLimitedInformation, false, processId);

        if (process.IsInvalid)
        {
            return null;
        }

        Span<char> buffer = stackalloc char[1024];
        var length = (uint)buffer.Length;

        unsafe
        {
            fixed (char* pointer = buffer)
            {
                if (!QueryFullProcessImageName(process, 0, pointer, ref length))
                {
                    return null;
                }
            }
        }

        return new string(buffer[..(int)length]);
    }

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint processId);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    private static partial SafeProcessHandle OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inheritHandle, uint processId);

    [LibraryImport("kernel32.dll", EntryPoint = "QueryFullProcessImageNameW", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static unsafe partial bool QueryFullProcessImageName(SafeProcessHandle process, uint flags, char* name, ref uint size);
}
