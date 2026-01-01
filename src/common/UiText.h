#pragma once

#include <filesystem>
#include <string>

namespace krkrspeed::ui_text {

enum class UiTextId {
    WindowTitle,
    LabelProcess,
    ButtonHook,
    ButtonHooked,
    LabelGamePath,
    ButtonLaunchHook,
    LabelSpeed,
    LabelProcessBgm,
    LabelAutoHook,
    LinkMarkup,
    LinkPlain,
    TooltipProcessCombo,
    TooltipHookButton,
    TooltipPathEdit,
    TooltipLaunchButton,
    TooltipSpeedEdit
};

bool LoadUiTextPacks(const std::filesystem::path &path, std::wstring &error);
void SetUiLanguage(const std::wstring &langCode);
const std::wstring &UiText(UiTextId id);

} // namespace krkrspeed::ui_text
