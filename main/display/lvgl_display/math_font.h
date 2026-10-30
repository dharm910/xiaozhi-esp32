#ifndef MATH_FONT_H
#define MATH_FONT_H

#include <lvgl.h>

// Returns a font that draws math and science symbols (√ ≤ ≥ ≠ ≈ ∞ → Δ Ω ∫ ∑ ✓ ...)
// from the generated font_stem_math_* glyphs and everything else from `base`, sharing its baseline.
// The result lives for the whole program; calls with the same base return the same font.
const lv_font_t* WithMathSymbols(const lv_font_t* base);

#endif  // MATH_FONT_H
