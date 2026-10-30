#ifndef STEM_BOARD_INTERNAL_H
#define STEM_BOARD_INTERNAL_H

// Helpers shared by the StemBoard implementation files; not part of its interface.

#include <cJSON.h>
#include <lvgl.h>

#include <string>

namespace stem_detail {

constexpr uint32_t kAutoCloseMs = 120000;
constexpr size_t kMaxTextLength = 120;

inline lv_color_t Accent() { return lv_palette_main(LV_PALETTE_BLUE); }
inline lv_color_t Success() { return lv_palette_main(LV_PALETTE_GREEN); }
inline lv_color_t Failure() { return lv_palette_main(LV_PALETTE_RED); }
inline lv_color_t Muted() { return lv_palette_main(LV_PALETTE_GREY); }

inline double NumberItem(const cJSON* obj, const char* key, double fallback) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

inline const char* StringItem(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

inline bool BoolItem(const cJSON* obj, const char* key, bool fallback) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

// Reads [x, y] from an array item.
inline bool ReadPoint(const cJSON* item, double& x, double& y) {
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) < 2) {
        return false;
    }
    const cJSON* xi = cJSON_GetArrayItem(item, 0);
    const cJSON* yi = cJSON_GetArrayItem(item, 1);
    if (!cJSON_IsNumber(xi) || !cJSON_IsNumber(yi)) {
        return false;
    }
    x = xi->valuedouble;
    y = yi->valuedouble;
    return true;
}

inline std::string Truncate(const char* text) {
    std::string s = text != nullptr ? text : "";
    if (s.size() > kMaxTextLength) {
        // Cut on a UTF-8 boundary
        size_t cut = kMaxTextLength;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
            cut--;
        }
        s.resize(cut);
    }
    return s;
}

inline void MakePlain(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

inline lv_obj_t* CreateLayer(lv_obj_t* parent) {
    lv_obj_t* layer = lv_obj_create(parent);
    MakePlain(layer);
    lv_obj_set_size(layer, lv_pct(100), lv_pct(100));
    return layer;
}

}  // namespace stem_detail

#endif  // STEM_BOARD_INTERNAL_H
