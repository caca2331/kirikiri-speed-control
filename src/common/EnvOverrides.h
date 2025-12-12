#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace krkrspeed {

// Key/value pair for environment overrides (wide strings to match Win32 APIs).
using EnvPair = std::pair<std::wstring, std::wstring>;

// Location of the shared override file (temp directory).
std::filesystem::path EnvOverrideFilePath();

// Write overrides to the shared file (UTF-8). Overwrites existing file.
void WriteEnvOverrides(const std::vector<EnvPair> &overrides);

// Apply overrides from the shared file into the current process environment.
// Best-effort: silently returns on error or missing file.
void ApplyEnvOverridesFromFile();

} // namespace krkrspeed
