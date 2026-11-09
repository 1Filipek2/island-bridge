using System.Globalization;
using System.Reflection;

namespace IslandBridge.Core.Updates;

public readonly record struct UpdateVersion(int Major, int Minor, int Patch) : IComparable<UpdateVersion>
{
    private const int MaxPart = 9999;

    public static UpdateVersion Current { get; } = FromAssembly(typeof(UpdateVersion).Assembly);

    public static bool TryParse(ReadOnlySpan<char> text, out UpdateVersion version)
    {
        version = default;
        Span<int> parts = stackalloc int[3];

        for (var index = 0; index < parts.Length; index++)
        {
            var dot = text.IndexOf('.');
            var last = index == parts.Length - 1;

            if (last == (dot >= 0))
            {
                return false;
            }

            var part = last ? text : text[..dot];

            if (!TryParsePart(part, out parts[index]))
            {
                return false;
            }

            text = last ? [] : text[(dot + 1)..];
        }

        version = new UpdateVersion(parts[0], parts[1], parts[2]);
        return true;
    }

    public int CompareTo(UpdateVersion other)
    {
        var major = Major.CompareTo(other.Major);

        if (major != 0)
        {
            return major;
        }

        var minor = Minor.CompareTo(other.Minor);
        return minor != 0 ? minor : Patch.CompareTo(other.Patch);
    }

    public static bool operator >(UpdateVersion left, UpdateVersion right) => left.CompareTo(right) > 0;

    public static bool operator <(UpdateVersion left, UpdateVersion right) => left.CompareTo(right) < 0;

    public static bool operator >=(UpdateVersion left, UpdateVersion right) => left.CompareTo(right) >= 0;

    public static bool operator <=(UpdateVersion left, UpdateVersion right) => left.CompareTo(right) <= 0;

    public override string ToString() => string.Create(CultureInfo.InvariantCulture, $"{Major}.{Minor}.{Patch}");

    // Digits only, no sign, no leading zeros: one spelling per version, so the signed file name is unambiguous.
    private static bool TryParsePart(ReadOnlySpan<char> part, out int value)
    {
        value = 0;

        if (part.Length is 0 or > 4 || (part.Length > 1 && part[0] == '0'))
        {
            return false;
        }

        foreach (var c in part)
        {
            if (!char.IsAsciiDigit(c))
            {
                return false;
            }

            value = (value * 10) + (c - '0');
        }

        return value <= MaxPart;
    }

    private static UpdateVersion FromAssembly(Assembly assembly)
    {
        var text = assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? string.Empty;
        var plus = text.IndexOf('+', StringComparison.Ordinal);
        return TryParse(plus >= 0 ? text.AsSpan(0, plus) : text, out var version) ? version : default;
    }
}
