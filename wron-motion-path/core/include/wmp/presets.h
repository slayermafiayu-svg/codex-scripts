// Wron Motion Path — built-in path presets.
// Every preset is an ordinary editable path (anchor points + handles), built
// in display space for the given frame aspect so circles are round on screen.
#pragma once

#include <string>

#include "wmp/path.h"

namespace wmp {

enum class PresetId { Straight, Arc, Circle, Ellipse, SCurve, Zigzag, Spiral };
inline constexpr int kPresetCount = 7;

const char* presetKey(PresetId id);  // stable key: "straight", "arc", ...
bool presetFromKey(const std::string& key, PresetId& out);

Path makePreset(PresetId id, double aspect);

}  // namespace wmp
