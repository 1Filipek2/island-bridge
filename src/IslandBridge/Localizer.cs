using System.Globalization;
using IslandBridge.Core.Configuration;
using Microsoft.Windows.ApplicationModel.Resources;

namespace IslandBridge;

internal sealed class Localizer
{
    private readonly ResourceManager _manager;
    private readonly ResourceMap _map;
    private ResourceContext _context;

    public Localizer(AppLanguage language)
    {
        _manager = new ResourceManager(Path.Combine(AppContext.BaseDirectory, "IslandBridge.pri"));
        _map = _manager.MainResourceMap.GetSubtree("Resources");
        _context = CreateContext(language);
    }

    public event EventHandler? LanguageChanged;

    public AppLanguage Language { get; private set; }

    public string this[string key]
    {
        get
        {
            var candidate = _map.TryGetValue(key, _context);
            return candidate?.ValueAsString ?? key;
        }
    }

    public string Format(string key, params object?[] args) => string.Format(CultureInfo.InvariantCulture, this[key], args);

    public void SetLanguage(AppLanguage language)
    {
        if (language == Language)
        {
            return;
        }

        _context = CreateContext(language);
        LanguageChanged?.Invoke(this, EventArgs.Empty);
    }

    private ResourceContext CreateContext(AppLanguage language)
    {
        Language = language;
        var context = _manager.CreateResourceContext();

        var tag = language switch
        {
            AppLanguage.English => "en-US",
            AppLanguage.Slovak => "sk-SK",
            _ => null,
        };

        if (tag is not null)
        {
            context.QualifierValues["Language"] = tag;
        }

        return context;
    }
}
