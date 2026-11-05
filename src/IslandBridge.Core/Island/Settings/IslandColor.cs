namespace IslandBridge.Core.Island.Settings;

public static class IslandColor
{
    public static string? Normalize(string value)
    {
        ArgumentNullException.ThrowIfNull(value);

        var digits = value.Trim().TrimStart('#');

        if (digits.Length is not (3 or 4 or 6 or 8) || !digits.All(char.IsAsciiHexDigit))
        {
            return null;
        }

        return "#" + digits.ToUpperInvariant();
    }

    public static bool TryParse(string value, out byte red, out byte green, out byte blue, out byte alpha)
    {
        red = green = blue = 0;
        alpha = 255;
        var normalized = Normalize(value);

        if (normalized is null)
        {
            return false;
        }

        var digits = normalized[1..];

        if (digits.Length <= 4)
        {
            digits = string.Concat(digits.Select(digit => new string(digit, 2)));
        }

        red = Convert.ToByte(digits[..2], 16);
        green = Convert.ToByte(digits[2..4], 16);
        blue = Convert.ToByte(digits[4..6], 16);
        alpha = digits.Length == 8 ? Convert.ToByte(digits[6..8], 16) : (byte)255;
        return true;
    }

    public static string Format(byte red, byte green, byte blue, byte alpha, bool withAlpha) =>
        withAlpha && alpha != 255 ? $"#{red:X2}{green:X2}{blue:X2}{alpha:X2}" : $"#{red:X2}{green:X2}{blue:X2}";
}
