#include "math_font.h"

#include <esp_log.h>

#define TAG "MathFont"

LV_FONT_DECLARE(font_stem_math_20_4);
LV_FONT_DECLARE(font_stem_math_30_4);

namespace {

struct ComposedFont {
    const lv_font_t* base;
    lv_font_t font;
};

constexpr int kMaxComposedFonts = 4;
ComposedFont composed_fonts[kMaxComposedFonts];
int composed_count = 0;

}  // namespace

const lv_font_t* WithMathSymbols(const lv_font_t* base) {
    if (base == nullptr) {
        return nullptr;
    }
    // Callers hold the display lock, which also serializes access to this cache.
    for (int i = 0; i < composed_count; i++) {
        if (composed_fonts[i].base == base) {
            // An asset font may have been freed and another allocated at the same address
            composed_fonts[i].font.line_height = base->line_height;
            composed_fonts[i].font.base_line = base->base_line;
            return &composed_fonts[i].font;
        }
    }
    if (composed_count == kMaxComposedFonts) {
        ESP_LOGW(TAG, "Composed font cache is full, math symbols unavailable for this font");
        return base;
    }
    auto& entry = composed_fonts[composed_count++];
    entry.base = base;
    // Pick the symbol size closest to the base font: the 20px base has a 28px line, 30px has 43.
    entry.font = base->line_height >= 36 ? font_stem_math_30_4 : font_stem_math_20_4;
    entry.font.line_height = base->line_height;
    entry.font.base_line = base->base_line;
    entry.font.fallback = base;
    return &entry.font;
}
