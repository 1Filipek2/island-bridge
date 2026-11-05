#include "core/localization.h"

namespace island
{

// "Playing from %s" + "Brave" -> "Playing from Brave"
std::wstring format_with_name(const wchar_t* format, const std::wstring& name)
{
    std::wstring out = format;
    const size_t pos = out.find(L"%s");
    if (pos != std::wstring::npos)
    {
        out.replace(pos, 2, name);
    }
    return out;
}

UiLanguage language_from_tag(std::wstring_view tag)
{
    struct Entry
    {
        const wchar_t* tag;
        UiLanguage lang;
    };

    static constexpr Entry k_tags[] = {
        {L"en", UiLanguage::English},    {L"fr", UiLanguage::French},
        {L"es", UiLanguage::Spanish},    {L"de", UiLanguage::German},
        {L"pt", UiLanguage::Portuguese}, {L"it", UiLanguage::Italian},
        {L"ru", UiLanguage::Russian},    {L"tr", UiLanguage::Turkish},
        {L"hi", UiLanguage::Hindi},      {L"zh", UiLanguage::ChineseSimplified},
        {L"ja", UiLanguage::Japanese},   {L"ko", UiLanguage::Korean},
    };
    if (tag.size() < 2)
    {
        return UiLanguage::English;
    }
    for (const Entry& e : k_tags)
    {
        // Prefix match so "pt-BR" / "zh-Hans-CN" resolve to their base language.
        if (_wcsnicmp(tag.data(), e.tag, 2) == 0)
        {
            return e.lang;
        }
    }
    return UiLanguage::English;
}

// Resolves the "auto" language setting from the user's Windows UI language.
UiLanguage detect_system_language()
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(name, ARRAYSIZE(name)) > 0)
    {
        return language_from_tag(name);
    }
    return UiLanguage::English;
}

// ── Clock / date presentation (#61) ──────────────────────────────────────────

// Formats the time honouring the 12h/24h override and the seconds toggle.
// Windows' own locale formatting is used unless the user forced a mode, so the
// default keeps regional conventions (separators, AM/PM placement) intact.
std::wstring format_island_time(const SYSTEMTIME& local, bool follow_system, bool use24_hour, bool show_seconds)
{
    wchar_t buffer[64] = {};

    if (follow_system)
    {
        const DWORD flags = show_seconds ? 0 : TIME_NOSECONDS;
        if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &local, nullptr, buffer, ARRAYSIZE(buffer)) > 0)
        {
            return buffer;
        }
        return L"--:--";
    }

    // Explicit override: build the pattern rather than fighting locale flags.
    const wchar_t* pattern =
        use24_hour ? (show_seconds ? L"HH:mm:ss" : L"HH:mm") : (show_seconds ? L"h:mm:ss tt" : L"h:mm tt");
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, pattern, buffer, ARRAYSIZE(buffer)) > 0)
    {
        return buffer;
    }
    return L"--:--";
}

// Formats the date. A custom pattern is passed straight to Windows, which
// already supports yyyy / MM / dd / MMM / MMMM / ddd / dddd and prints anything
// else literally -- so CJK patterns such as yyyy年MM月dd日 work as typed.
std::wstring format_island_date(const SYSTEMTIME& local, const std::wstring& custom_format,
                                const wchar_t* fallback_pattern)
{
    wchar_t buffer[128] = {};

    if (!custom_format.empty())
    {
        if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, custom_format.c_str(), buffer, ARRAYSIZE(buffer),
                            nullptr) > 0)
        {
            return buffer;
        }
        // An invalid pattern falls through to the default rather than showing
        // nothing at all.
    }

    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, fallback_pattern, buffer, ARRAYSIZE(buffer), nullptr) > 0)
    {
        return buffer;
    }
    return std::wstring();
}

const wchar_t* loc(const wchar_t* english)
{
    struct Row
    {
        const wchar_t* key;
        // Indexed by UiLanguage; nullptr falls back to the English key.
        const wchar_t* text[static_cast<size_t>(UiLanguage::Count)];
    };

    // Order: en, fr, es, de, pt, it, ru, tr, hi, zh, ja, ko
    static const Row k_rows[] = {
        {L"Media",
         {nullptr, L"Média", L"Multimedia", L"Medien", L"Mídia", L"Media", L"Медиа", L"Medya", L"मीडिया", L"媒体",
          L"メディア", L"미디어"}},
        {L"Calendar",
         {nullptr, L"Calendrier", L"Calendario", L"Kalender", L"Calendário", L"Calendario", L"Календарь", L"Takvim",
          L"कैलेंडर", L"日历", L"カレンダー", L"캘린더"}},
        {L"Weather",
         {nullptr, L"Météo", L"Tiempo", L"Wetter", L"Tempo", L"Meteo", L"Погода", L"Hava", L"मौसम", L"天气", L"天気",
          L"날씨"}},
        {L"Hardware Monitor",
         {nullptr, L"Moniteur matériel", L"Monitor de hardware", L"Hardware-Monitor", L"Monitor de hardware",
          L"Monitor hardware", L"Монитор системы", L"Donanım İzleme", L"हार्डवेयर मॉनिटर", L"硬件监视器",
          L"ハードウェア モニター", L"하드웨어 모니터"}},
        {L"File Tray",
         {nullptr, L"Bac à fichiers", L"Bandeja de archivos", L"Dateiablage", L"Bandeja de arquivos", L"Vassoio file",
          L"Файлы", L"Dosya Tepsisi", L"फ़ाइल ट्रे", L"文件托盘", L"ファイル トレイ", L"파일 트레이"}},
        {L"Bluetooth",
         {nullptr, L"Bluetooth", L"Bluetooth", L"Bluetooth", L"Bluetooth", L"Bluetooth", L"Bluetooth", L"Bluetooth",
          L"ब्लूटूथ", L"蓝牙", L"Bluetooth", L"블루투스"}},
        {L"Copied",
         {nullptr, L"Copié", L"Copiado", L"Kopiert", L"Copiado", L"Copiato", L"Скопировано", L"Kopyalandı",
          L"कॉपी किया गया", L"已复制", L"コピーしました", L"복사됨"}},
        {L"Volume",
         {nullptr, L"Volume", L"Volumen", L"Lautstärke", L"Volume", L"Volume", L"Громкость", L"Ses", L"वॉल्यूम", L"音量",
          L"音量", L"볼륨"}},
        {L"Brightness",
         {nullptr, L"Luminosité", L"Brillo", L"Helligkeit", L"Brilho", L"Luminosità", L"Яркость", L"Parlaklık", L"चमक",
          L"亮度", L"明るさ", L"밝기"}},
        {L"Muted",
         {nullptr, L"Muet", L"Silenciado", L"Stumm", L"Sem som", L"Muto", L"Без звука", L"Sessiz", L"म्यूट", L"已静音",
          L"ミュート", L"음소거"}},
        {L"Low Battery",
         {nullptr, L"Batterie faible", L"Batería baja", L"Akku schwach", L"Bateria fraca", L"Batteria scarica",
          L"Батарея разряжена", L"Pil Az", L"बैटरी कम", L"电量低", L"バッテリー残量低下", L"배터리 부족"}},
        {L"Charging",
         {nullptr, L"En charge", L"Cargando", L"Wird geladen", L"Carregando", L"In carica", L"Зарядка", L"Şarj oluyor",
          L"चार्ज हो रहा है", L"正在充电", L"充電中", L"충전 중"}},
        {L"Connected",
         {nullptr, L"Connecté", L"Conectado", L"Verbunden", L"Conectado", L"Connesso", L"Подключено", L"Bağlandı",
          L"कनेक्ट किया गया", L"已连接", L"接続済み", L"연결됨"}},
        {L"Disconnected",
         {nullptr, L"Déconnecté", L"Desconectado", L"Getrennt", L"Desconectado", L"Disconnesso", L"Отключено",
          L"Bağlantı kesildi", L"डिस्कनेक्ट किया गया", L"已断开", L"切断されました", L"연결 끊김"}},
        {L"Caps Lock",
         {nullptr, L"Verr. Maj", L"Bloq Mayús", L"Feststelltaste", L"Caps Lock", L"Blocco maiuscole", L"Caps Lock",
          L"Caps Lock", L"कैप्स लॉक", L"大写锁定", L"Caps Lock", L"Caps Lock"}},
        {L"Num Lock",
         {nullptr, L"Verr. Num", L"Bloq Num", L"Num-Taste", L"Num Lock", L"Blocco num", L"Num Lock", L"Num Lock",
          L"नम लॉक", L"数字锁定", L"Num Lock", L"Num Lock"}},
        {L"On",
         {nullptr, L"Activé", L"Activado", L"Ein", L"Ligado", L"Attivo", L"Вкл", L"Açık", L"चालू", L"开", L"オン",
          L"켜짐"}},
        {L"Off",
         {nullptr, L"Désactivé", L"Desactivado", L"Aus", L"Desligado", L"Disattivo", L"Выкл", L"Kapalı", L"बंद", L"关",
          L"オフ", L"꺼짐"}},
        {L"Do Not Disturb",
         {nullptr, L"Ne pas déranger", L"No molestar", L"Nicht stören", L"Não perturbe", L"Non disturbare",
          L"Не беспокоить", L"Rahatsız Etme", L"परेशान न करें", L"专注助手", L"応答不可", L"방해 금지"}},
        {L"Focus",
         {nullptr, L"Concentration", L"Concentración", L"Fokus", L"Foco", L"Concentrazione", L"Фокус", L"Odak", L"फ़ोकस",
          L"专注", L"集中", L"집중"}},
        {L"Break",
         {nullptr, L"Pause", L"Descanso", L"Pause", L"Pausa", L"Pausa", L"Перерыв", L"Mola", L"विराम", L"休息", L"休憩",
          L"휴식"}},
        {L"Unknown",
         {nullptr, L"Inconnu", L"Desconocido", L"Unbekannt", L"Desconhecido", L"Sconosciuto", L"Неизвестно",
          L"Bilinmiyor", L"अज्ञात", L"未知", L"不明", L"알 수 없음"}},
        {L"Loading...",
         {nullptr, L"Chargement...", L"Cargando...", L"Wird geladen...", L"Carregando...", L"Caricamento...",
          L"Загрузка...", L"Yükleniyor...", L"लोड हो रहा है...", L"加载中...", L"読み込み中...", L"불러오는 중..."}},
        {L"Locating...",
         {nullptr, L"Localisation...", L"Ubicando...", L"Standort...", L"Localizando...", L"Localizzazione...",
          L"Определение...", L"Konum...", L"स्थान...", L"定位中...", L"位置情報...", L"위치 확인 중..."}},
        {L"Wind",
         {nullptr, L"Vent", L"Viento", L"Wind", L"Vento", L"Vento", L"Ветер", L"Rüzgar", L"हवा", L"风速", L"風",
          L"바람"}},
        {L"Feels Like",
         {nullptr, L"Ressenti", L"Sensación", L"Gefühlt", L"Sensação", L"Percepita", L"Ощущается", L"Hissedilen",
          L"महसूस", L"体感", L"体感", L"체감"}},
        {L"Humidity",
         {nullptr, L"Humidité", L"Humedad", L"Luftfeuchte", L"Umidade", L"Umidità", L"Влажность", L"Nem", L"नमी",
          L"湿度", L"湿度", L"습도"}},
        {L"Drag files here",
         {nullptr, L"Déposez des fichiers ici", L"Arrastra archivos aquí", L"Dateien hierher ziehen",
          L"Arraste arquivos aqui", L"Trascina i file qui", L"Перетащите файлы сюда", L"Dosyaları buraya sürükleyin",
          L"फ़ाइलें यहाँ खींचें", L"将文件拖到此处", L"ここにファイルをドラッグ", L"여기에 파일을 끌어다 놓으세요"}},
        {L"No devices",
         {nullptr, L"Aucun appareil", L"Sin dispositivos", L"Keine Geräte", L"Nenhum dispositivo",
          L"Nessun dispositivo", L"Нет устройств", L"Cihaz yok", L"कोई डिवाइस नहीं", L"无设备", L"デバイスなし",
          L"장치 없음"}},
        {L"item",
         {nullptr, L"élément", L"elemento", L"Element", L"item", L"elemento", L"элемент", L"öğe", L"आइटम", L"项",
          L"項目", L"항목"}},
        {L"items",
         {nullptr, L"éléments", L"elementos", L"Elemente", L"itens", L"elementi", L"элементов", L"öğe", L"आइटम", L"项",
          L"項目", L"항목"}},
        {L"Media source",
         {nullptr, L"Source multimédia", L"Fuente multimedia", L"Medienquelle", L"Fonte de mídia",
          L"Sorgente multimediale", L"Источник медиа", L"Medya kaynağı", L"मीडिया स्रोत", L"媒体来源",
          L"メディア ソース", L"미디어 소스"}},
        {L"Auto",
         {nullptr, L"Auto", L"Automático", L"Automatisch", L"Automático", L"Automatico", L"Авто", L"Otomatik",
          L"स्वचालित", L"自动", L"自動", L"자동"}},
        {L"Playing from %s",
         {nullptr, L"Lecture depuis %s", L"Reproduciendo desde %s", L"Wiedergabe über %s", L"Reproduzindo em %s",
          L"In riproduzione da %s", L"Воспроизводится в %s", L"%s üzerinden oynatılıyor", L"%s से चल रहा है",
          L"正在 %s 中播放", L"%s で再生中", L"%s에서 재생 중"}},
        {L"Paused in %s",
         {nullptr, L"En pause dans %s", L"En pausa en %s", L"Pausiert in %s", L"Pausado em %s", L"In pausa in %s",
          L"Приостановлено в %s", L"%s içinde duraklatıldı", L"%s में रोका गया", L"已在 %s 中暂停", L"%s で一時停止中",
          L"%s에서 일시 중지됨"}},
    };

    const int lang_index = g_ui_language.load(std::memory_order_relaxed);
    if (lang_index <= static_cast<int>(UiLanguage::English) || lang_index >= static_cast<int>(UiLanguage::Count))
    {
        return english;
    }

    for (const Row& row : k_rows)
    {
        if (wcscmp(row.key, english) == 0)
        {
            const wchar_t* translated = row.text[static_cast<size_t>(lang_index)];
            return translated ? translated : english;
        }
    }
    return english;
}

}
