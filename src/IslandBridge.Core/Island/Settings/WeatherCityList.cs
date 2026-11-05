using System.Globalization;
using IslandBridge.Core.Text;

namespace IslandBridge.Core.Island.Settings;

public sealed record WeatherCity(string Name, double Latitude, double Longitude);

public static class WeatherCityList
{
    public const int MaxCities = 16;
    public const int MaxNameLength = 48;

    // Same format and limits as the island's parser: Name|latitude|longitude, separated by semicolons.
    public static bool TryParse(string value, out List<WeatherCity> cities)
    {
        ArgumentNullException.ThrowIfNull(value);
        cities = [];

        foreach (var item in value.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var parts = item.Split('|');

            if (parts.Length != 3 || cities.Count == MaxCities)
            {
                return false;
            }

            var city = Create(parts[0], parts[1], parts[2]);

            if (city is null)
            {
                return false;
            }

            cities.Add(city);
        }

        return true;
    }

    public static WeatherCity? Create(string name, string latitude, string longitude)
    {
        ArgumentNullException.ThrowIfNull(name);
        ArgumentNullException.ThrowIfNull(latitude);
        ArgumentNullException.ThrowIfNull(longitude);

        var cleanName = CleanName(name);

        if (cleanName.Length == 0 ||
            !TryParseCoordinate(latitude, 90, out var lat) ||
            !TryParseCoordinate(longitude, 180, out var lon))
        {
            return null;
        }

        return new WeatherCity(cleanName, lat, lon);
    }

    public static string Format(IEnumerable<WeatherCity> cities)
    {
        ArgumentNullException.ThrowIfNull(cities);
        return string.Join(';', cities.Take(MaxCities).Select(city => string.Create(
            CultureInfo.InvariantCulture,
            $"{CleanName(city.Name)}|{Math.Round(city.Latitude, 5):0.#####}|{Math.Round(city.Longitude, 5):0.#####}")));
    }

    private static string CleanName(string name) =>
        TextSanitizer.Sanitize(name.Replace('|', ' ').Replace(';', ' '), MaxNameLength).Trim();

    private static bool TryParseCoordinate(string text, double limit, out double value)
    {
        var normalized = text.Trim().Replace(',', '.');
        return double.TryParse(normalized, NumberStyles.AllowLeadingSign | NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out value) &&
               double.IsFinite(value) && value >= -limit && value <= limit;
    }
}
