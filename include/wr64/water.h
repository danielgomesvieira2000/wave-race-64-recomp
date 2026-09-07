#pragma once
#include <cstdint>
#include "shared/rt64_water_params.h"

namespace wr64::water {
enum class Quality : uint32_t { Original, Modern, High, Ultra };
enum class Style : uint32_t { Modern, Classic };
enum class RippleDetail : uint32_t { Soft, Normal, Strong };
// Publish on the original game's task submission thread; consume by list identity.
void publish_frame(const uint8_t *rdram, uint32_t display_list);
void reset_for_race();
void begin_frame(uint32_t display_list);
interop::WaterMaterial material(uint32_t view);
void toggle();
void cycle_debug();
Quality quality();
void set_quality(Quality value);
void set_style(Style value);
void set_ripple_detail(RippleDetail value);
void set_spray_enabled(bool enabled);
}
