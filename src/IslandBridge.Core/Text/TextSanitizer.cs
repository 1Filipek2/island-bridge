using System.Text;

namespace IslandBridge.Core.Text;

public static class TextSanitizer
{
    public static string Sanitize(ReadOnlySpan<byte> utf8, int maxLength)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(maxLength);
        return utf8.IsEmpty || maxLength == 0 ? string.Empty : Sanitize(Encoding.UTF8.GetString(utf8), maxLength);
    }

    public static string Sanitize(string text, int maxLength)
    {
        ArgumentNullException.ThrowIfNull(text);
        ArgumentOutOfRangeException.ThrowIfNegative(maxLength);

        if (text.Length == 0 || maxLength == 0)
        {
            return string.Empty;
        }

        var builder = new StringBuilder(Math.Min(text.Length, maxLength));
        var pendingSpace = false;

        foreach (var rune in text.EnumerateRunes())
        {
            if (IsWhitespaceSeparator(rune))
            {
                pendingSpace = builder.Length > 0;
                continue;
            }

            if (Rune.IsControl(rune) || IsBidiControl(rune.Value) || IsRepeatedReplacement(rune, builder))
            {
                continue;
            }

            var needed = rune.Utf16SequenceLength + (pendingSpace ? 1 : 0);

            if (builder.Length + needed > maxLength)
            {
                break;
            }

            if (pendingSpace)
            {
                builder.Append(' ');
                pendingSpace = false;
            }

            AppendRune(builder, rune);
        }

        return builder.ToString();
    }

    private static bool IsWhitespaceSeparator(Rune rune) =>
        rune.Value is '\n' or '\r' or '\t' or '\v' or '\f' or 0x85 or 0x2028 or 0x2029 || Rune.IsWhiteSpace(rune);

    private static bool IsBidiControl(int value) =>
        value is (>= 0x202A and <= 0x202E) or (>= 0x2066 and <= 0x2069) or 0x200E or 0x200F or 0x061C;

    private static bool IsRepeatedReplacement(Rune rune, StringBuilder builder) =>
        rune == Rune.ReplacementChar && builder.Length > 0 && builder[^1] == (char)Rune.ReplacementChar.Value;

    private static void AppendRune(StringBuilder builder, Rune rune)
    {
        Span<char> buffer = stackalloc char[2];
        var written = rune.EncodeToUtf16(buffer);
        builder.Append(buffer[..written]);
    }
}
