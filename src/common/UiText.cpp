#include "UiText.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace krkrspeed::ui_text {
namespace {

struct UiTextIdHash {
    std::size_t operator()(UiTextId id) const noexcept {
        return static_cast<std::size_t>(id);
    }
};

struct UiTextPack {
    std::unordered_map<UiTextId, std::wstring, UiTextIdHash> texts;
};

struct KeyEntry {
    UiTextId id;
    const char *key;
    const wchar_t *fallback;
};

const KeyEntry kEntries[] = {
    {UiTextId::WindowTitle, "window.title", L""},
    {UiTextId::LabelProcess, "label.process", L""},
    {UiTextId::ButtonHook, "button.hook", L""},
    {UiTextId::ButtonHooked, "button.hooked", L""},
    {UiTextId::LabelSpeed, "label.speed", L""},
    {UiTextId::LabelProcessBgm, "label.process_bgm", L""},
    {UiTextId::LabelAutoHook, "label.auto_hook", L""},
    {UiTextId::LabelAutoHookDelay, "label.auto_hook_delay", L""},
    {UiTextId::LabelHotkey, "label.hotkey", L""},
    {UiTextId::LinkMarkup, "link.markup", L""},
    {UiTextId::LinkPlain, "link.plain", L""},
    {UiTextId::TooltipProcessCombo, "tooltip.process_combo", L""},
    {UiTextId::TooltipHookButton, "tooltip.hook_button", L""},
    {UiTextId::TooltipSpeedEdit, "tooltip.speed_edit", L""},
    {UiTextId::TooltipProcessBgm, "tooltip.process_bgm", L""},
    {UiTextId::TooltipAutoHook, "tooltip.auto_hook", L""},
    {UiTextId::TooltipAutoHookDelay, "tooltip.auto_hook_delay", L""},
    {UiTextId::TooltipHotkey, "tooltip.hotkey", L""}
};

std::unordered_map<std::wstring, UiTextPack> g_packs;
std::unordered_map<UiTextId, std::wstring, UiTextIdHash> g_fallback;
std::wstring g_currentLang = L"en";
bool g_fallbackReady = false;

std::string trimCopy(const std::string &input) {
    const auto start = input.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = input.find_last_not_of(" \t\r\n");
    return input.substr(start, end - start + 1);
}

std::string unescapeQuoted(const std::string &input) {
    std::string out;
    out.reserve(input.size());
    bool escape = false;
    for (char c : input) {
        if (escape) {
            switch (c) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(c); break;
            }
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::wstring fromUtf8(const std::string &input) {
    if (input.empty()) {
        return {};
    }
#ifdef _WIN32
    const int sizeNeeded =
        MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (sizeNeeded <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(sizeNeeded), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), sizeNeeded);
    return output;
#else
    return std::wstring(input.begin(), input.end());
#endif
}

bool lookupId(const std::string &key, UiTextId &id) {
    for (const auto &entry : kEntries) {
        if (key == entry.key) {
            id = entry.id;
            return true;
        }
    }
    return false;
}

void ensureFallback() {
    if (g_fallbackReady) {
        return;
    }
    for (const auto &entry : kEntries) {
        g_fallback.emplace(entry.id, entry.fallback ? entry.fallback : L"");
    }
    g_fallbackReady = true;
}

} // namespace

bool LoadUiTextPacks(const std::filesystem::path &path, std::wstring &error) {
    ensureFallback();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = L"Unable to open ui_texts.yaml: " + path.wstring();
        return false;
    }

    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (contents.rfind("\xEF\xBB\xBF", 0) == 0) {
        contents.erase(0, 3);
    }

    std::unordered_map<std::wstring, UiTextPack> packs;
    std::istringstream lines(contents);
    std::string line;
    std::string currentLang;

    while (std::getline(lines, line)) {
        std::size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) {
            ++indent;
        }
        const std::string trimmed = trimCopy(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        if (indent == 0 && trimmed.back() == ':') {
            currentLang = trimCopy(trimmed.substr(0, trimmed.size() - 1));
            continue;
        }

        if (currentLang.empty() || indent == 0) {
            continue;
        }

        const auto colon = trimmed.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = trimCopy(trimmed.substr(0, colon));
        std::string value = trimCopy(trimmed.substr(colon + 1));
        if (value.empty() || value[0] == '#') {
            value.clear();
        }

        if (!value.empty() && value.front() != '"' && value.front() != '\'') {
            const auto hashPos = value.find(" #");
            if (hashPos != std::string::npos) {
                value = trimCopy(value.substr(0, hashPos));
            }
        }

        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
            value = unescapeQuoted(value);
        }

        UiTextId id{};
        if (!lookupId(key, id)) {
            continue;
        }
        packs[fromUtf8(currentLang)].texts[id] = fromUtf8(value);
    }

    if (packs.empty()) {
        error = L"No language packs loaded from ui_texts.yaml.";
        return false;
    }

    g_packs = std::move(packs);
    return true;
}

void SetUiLanguage(const std::wstring &langCode) {
    g_currentLang = langCode;
}

const std::wstring &UiText(UiTextId id) {
    ensureFallback();
    auto itPack = g_packs.find(g_currentLang);
    if (itPack != g_packs.end()) {
        auto itText = itPack->second.texts.find(id);
        if (itText != itPack->second.texts.end()) {
            return itText->second;
        }
    }
    auto itFallback = g_fallback.find(id);
    if (itFallback != g_fallback.end()) {
        return itFallback->second;
    }
    static const std::wstring kEmpty;
    return kEmpty;
}

} // namespace krkrspeed::ui_text
