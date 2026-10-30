// Story mode for StemBoard: a sequence of scenes, each with a caption the model speaks and
// actions that add, move and transform named objects. Objects persist across scenes, so a
// story reads as one continuous animation. Scenes advance when the matching sentence starts
// playing (Application forwards each TTS sentence), with a timer as fallback.

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "display.h"
#include "stem_board.h"
#include "stem_board_internal.h"

#define TAG "StemStory"

using namespace stem_detail;

namespace {

constexpr int kMaxScenes = 12;
constexpr int kMaxActionsPerScene = 24;
constexpr size_t kMaxStoryObjects = 40;
constexpr int kMaxPathPoints = 16;
constexpr int kIconBox = 44;
constexpr uint32_t kDefaultActionMs = 800;
constexpr uint32_t kMaxActionMs = 20000;
constexpr uint32_t kMinSceneMs = 2500;
constexpr uint32_t kMsPerCaptionChar = 70;
constexpr uint32_t kStoryPlayingCloseMs = 600000;
constexpr uint32_t kStoryEndCloseMs = 60000;
constexpr uint32_t kSyncedHoldFactor = 3;
constexpr uint32_t kMinSceneBeforeSkipMs = 1500;
constexpr float kSentenceMatch = 0.6f;

struct GlyphIcon {
    const char* name;
    const char* utf8;  // glyph in font_stem_icons_40_4
    lv_palette_t palette;
    bool text_color;  // draw in the theme text color instead of the palette
};

const GlyphIcon kGlyphIcons[] = {
    {"sun", "☀", LV_PALETTE_AMBER, false},
    {"cloud", "☁", LV_PALETTE_BLUE_GREY, false},
    {"rain", "☔", LV_PALETTE_BLUE, false},
    {"snowflake", "❄", LV_PALETTE_LIGHT_BLUE, false},
    {"lightning", "⚡", LV_PALETTE_YELLOW, false},
    {"star", "★", LV_PALETTE_YELLOW, false},
    {"moon", "☾", LV_PALETTE_AMBER, false},
    {"snowman", "☃", LV_PALETTE_LIGHT_BLUE, false},
    {"leaf", "☘", LV_PALETTE_GREEN, false},
    {"heart", "♥", LV_PALETTE_RED, false},
    {"smile", "☺", LV_PALETTE_AMBER, false},
    {"house", "⌂", LV_PALETTE_BROWN, false},
    {"gear", "⚙", LV_PALETTE_GREY, false},
    {"atom", "⚛", LV_PALETTE_CYAN, false},
    {"plane", "✈", LV_PALETTE_BLUE, false},
    {"flag", "⚑", LV_PALETTE_RED, false},
    {"crown", "♚", LV_PALETTE_AMBER, false},
    {"music", "♪", LV_PALETTE_PURPLE, false},
    {"recycle", "♻", LV_PALETTE_GREEN, false},
    {"warning", "⚠", LV_PALETTE_AMBER, false},
    {"radiation", "☢", LV_PALETTE_YELLOW, false},
    {"anchor", "⚓", LV_PALETTE_BLUE_GREY, false},
    {"cup", "☕", LV_PALETTE_BROWN, false},
    {"pencil", "✏", LV_PALETTE_ORANGE, false},
    {"check", "✔", LV_PALETTE_GREEN, false},
    {"female", "♀", LV_PALETTE_PINK, false},
    {"male", "♂", LV_PALETTE_BLUE, false},
    {"scales", "⚖", LV_PALETTE_GREY, true},
    {"scissors", "✂", LV_PALETTE_GREY, true},
    {"envelope", "✉", LV_PALETTE_GREY, true},
    {"phone", "☎", LV_PALETTE_GREY, true},
    {"point", "☛", LV_PALETTE_GREY, true},
    {"skull", "☠", LV_PALETTE_GREY, true},
};

// Icons built from shapes, for pictures the font does not have. See BuildIcon().
const char* const kDrawnIcons[] = {"flower", "drop",    "fire",   "tree", "plant", "person",
                                   "bulb",   "battery", "magnet", "car",  "earth",
                                   "cell",  "mountain", "molecule", "ball"};

const GlyphIcon* FindGlyphIcon(const char* name) {
    for (const auto& icon : kGlyphIcons) {
        if (strcmp(icon.name, name) == 0) {
            return &icon;
        }
    }
    return nullptr;
}

bool IsDrawnIcon(const char* name) {
    for (const char* icon : kDrawnIcons) {
        if (strcmp(icon, name) == 0) {
            return true;
        }
    }
    return false;
}

bool IsShapeType(const char* type) {
    static const char* const kTypes[] = {"line", "polygon", "arrow", "rect",
                                         "circle", "dot", "arc"};
    for (const char* t : kTypes) {
        if (strcmp(t, type) == 0) {
            return true;
        }
    }
    return false;
}

void ExecX(void* obj, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t*>(obj), v); }
void ExecY(void* obj, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(obj), v); }
void ExecOpa(void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}
void ExecScale(void* obj, int32_t v) {
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), v, 0);
}

void RunAnim(lv_obj_t* obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms,
             lv_anim_path_cb_t path = lv_anim_path_ease_in_out, uint32_t repeat = 0,
             uint32_t reverse_ms = 0) {
    lv_anim_delete(obj, exec);
    if (ms == 0) {
        exec(obj, to);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    if (repeat > 0) {
        lv_anim_set_repeat_count(&a, repeat);
    }
    if (reverse_ms > 0) {
        lv_anim_set_reverse_duration(&a, reverse_ms);
    }
    lv_anim_start(&a);
}

}  // namespace

namespace {

// Places obj along a polyline; v runs from 0 to 1000. A template because PathData is private.
template <typename Path>
void MoveAlong(lv_obj_t* obj, const Path& path, int32_t v) {
    float total = path.lengths.back();
    float distance = total * v / 1000.0f;
    size_t i = 1;
    while (i < path.points.size() - 1 && path.lengths[i] < distance) {
        i++;
    }
    float seg = path.lengths[i] - path.lengths[i - 1];
    float t = seg > 0 ? (distance - path.lengths[i - 1]) / seg : 1.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const auto& a = path.points[i - 1];
    const auto& b = path.points[i];
    int x = a.x + static_cast<int>((b.x - a.x) * t);
    int y = a.y + static_cast<int>((b.y - a.y) * t);
    lv_obj_set_pos(obj, x - path.off_x, y - path.off_y);
}

std::vector<std::string> Words(const char* text) {
    std::vector<std::string> words;
    std::string current;
    for (const char* p = text; p != nullptr && *p != '\0'; p++) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (isalnum(c) || c >= 0x80) {
            current += static_cast<char>(tolower(c));
        } else if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    return words;
}

// Share of the shorter text's words that also appear in the other text. Server sentences are
// often fragments of a scene's caption, so the shorter side decides.
float Overlap(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    size_t shorter = std::min(a.size(), b.size());
    if (shorter == 0) {
        return 0;
    }
    std::vector<std::string> common;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(common));
    if (shorter < 2) {
        return a == b ? 1.0f : 0.0f;
    }
    return static_cast<float>(common.size()) / shorter;
}

void Recolor(lv_obj_t* obj, lv_color_t color) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_obj_set_style_text_color(obj, color, 0);
    } else if (lv_obj_check_type(obj, &lv_line_class)) {
        lv_obj_set_style_line_color(obj, color, 0);
    } else if (lv_obj_check_type(obj, &lv_arc_class)) {
        lv_obj_set_style_arc_color(obj, color, LV_PART_MAIN);
    } else {
        if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) > LV_OPA_TRANSP) {
            lv_obj_set_style_bg_color(obj, color, 0);
        }
        if (lv_obj_get_style_border_width(obj, LV_PART_MAIN) > 0) {
            lv_obj_set_style_border_color(obj, color, 0);
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        Recolor(lv_obj_get_child(obj, i), color);
    }
}

void HideWhenDone(lv_anim_t* a) {
    lv_obj_add_flag(static_cast<lv_obj_t*>(a->var), LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

void StemBoard::ExecPathAnim(lv_anim_t* anim, int32_t v) {
    MoveAlong(static_cast<lv_obj_t*>(anim->var),
              *static_cast<const PathData*>(lv_anim_get_user_data(anim)), v);
}

std::string StemBoard::IconNames() {
    std::string names;
    for (const auto& icon : kGlyphIcons) {
        names += names.empty() ? "" : ", ";
        names += icon.name;
    }
    for (const char* icon : kDrawnIcons) {
        names += ", ";
        names += icon;
    }
    return names;
}

// ---------------------------------------------------------------------------------------------
// Validation: the whole story is checked before anything is shown, so the model gets one
// precise error instead of a half-played story.

std::string StemBoard::ValidateStory(const cJSON* scenes) const {
    if (!cJSON_IsArray(scenes) || cJSON_GetArraySize(scenes) == 0) {
        return "'scenes' must be a non-empty JSON array";
    }
    if (cJSON_GetArraySize(scenes) > kMaxScenes) {
        return "A story can have at most " + std::to_string(kMaxScenes) + " scenes";
    }
    struct Known {
        std::string id;
        char kind;  // 'i' icon, 't' text, 's' shape
    };
    std::vector<Known> live;
    size_t added = 0;
    int scene_number = 0;
    const cJSON* scene;
    cJSON_ArrayForEach(scene, scenes) {
        scene_number++;
        std::string where = "Scene " + std::to_string(scene_number);
        if (!cJSON_IsObject(scene)) {
            return where + " must be an object";
        }
        const cJSON* say = cJSON_GetObjectItem(scene, "say");
        if (say != nullptr && !cJSON_IsString(say)) {
            return where + ": 'say' must be a string";
        }
        const cJSON* actions = cJSON_GetObjectItem(scene, "actions");
        if (actions == nullptr) {
            continue;
        }
        if (!cJSON_IsArray(actions) || cJSON_GetArraySize(actions) > kMaxActionsPerScene) {
            return where + ": 'actions' must be an array of at most " +
                   std::to_string(kMaxActionsPerScene) + " actions";
        }
        int action_number = 0;
        const cJSON* action;
        cJSON_ArrayForEach(action, actions) {
            action_number++;
            std::string at = where + " action " + std::to_string(action_number);
            const char* op = StringItem(action, "do");
            const char* id = StringItem(action, "id");
            if (op == nullptr || id == nullptr) {
                return at + " needs 'do' and 'id' strings";
            }
            auto it = std::find_if(live.begin(), live.end(),
                                   [id](const Known& k) { return k.id == id; });
            if (strcmp(op, "add") == 0) {
                if (it != live.end()) {
                    return at + ": id '" + id + "' already exists";
                }
                if (++added > kMaxStoryObjects) {
                    return "A story can add at most " + std::to_string(kMaxStoryObjects) +
                           " objects";
                }
                const char* icon = StringItem(action, "icon");
                const char* type = StringItem(action, "type");
                char kind;
                if (icon != nullptr) {
                    if (FindGlyphIcon(icon) == nullptr && !IsDrawnIcon(icon)) {
                        return at + ": unknown icon '" + icon + "'. Icons: " + IconNames();
                    }
                    kind = 'i';
                } else if (type != nullptr && strcmp(type, "text") == 0) {
                    if (StringItem(action, "text") == nullptr) {
                        return at + ": text needs a 'text' string";
                    }
                    kind = 't';
                } else if (type != nullptr && IsShapeType(type)) {
                    kind = 's';
                } else {
                    return at + ": 'add' needs an 'icon', or a 'type' of text, line, polygon, "
                                "arrow, rect, circle, dot or arc";
                }
                live.push_back({id, kind});
                continue;
            }
            if (it == live.end()) {
                return at + ": unknown id '" + id + "' (add it in an earlier action first)";
            }
            double x, y;
            if (strcmp(op, "move") == 0) {
                if (!ReadPoint(cJSON_GetObjectItem(action, "to"), x, y)) {
                    return at + ": 'move' needs 'to': [x, y]";
                }
            } else if (strcmp(op, "path") == 0) {
                const cJSON* points = cJSON_GetObjectItem(action, "points");
                if (!cJSON_IsArray(points) || cJSON_GetArraySize(points) == 0 ||
                    !ReadPoint(cJSON_GetArrayItem(points, 0), x, y)) {
                    return at + ": 'path' needs 'points': [[x, y], ...]";
                }
            } else if (strcmp(op, "scale") == 0) {
                if (it->kind == 's') {
                    return at + ": 'scale' works on icons and text; use 'pulse' or 'fade' for "
                                "shapes";
                }
                if (!cJSON_IsNumber(cJSON_GetObjectItem(action, "to"))) {
                    return at + ": 'scale' needs a number 'to' (1 = normal size)";
                }
            } else if (strcmp(op, "fade") == 0) {
                if (!cJSON_IsNumber(cJSON_GetObjectItem(action, "to"))) {
                    return at + ": 'fade' needs a number 'to' from 0 (invisible) to 1";
                }
            } else if (strcmp(op, "color") == 0) {
                if (StringItem(action, "to") == nullptr) {
                    return at + ": 'color' needs a color name in 'to'";
                }
            } else if (strcmp(op, "text") == 0) {
                if (it->kind != 't' || StringItem(action, "text") == nullptr) {
                    return at + ": 'text' changes the 'text' of a text object";
                }
            } else if (strcmp(op, "remove") == 0) {
                live.erase(it);
            } else if (strcmp(op, "pulse") != 0) {
                return at + ": unknown 'do' '" + op +
                       "'. Use add, move, path, scale, fade, color, pulse, text or remove";
            }
        }
    }
    return "";
}

// ---------------------------------------------------------------------------------------------
// Playback

std::string StemBoard::Story(const std::string& title, const cJSON* scenes) {
    auto error = ValidateStory(scenes);
    if (!error.empty()) {
        return error;
    }
    CJsonUniquePtr copy(cJSON_Duplicate(scenes, true));
    if (copy == nullptr) {
        return "Out of memory";
    }
    DisplayLockGuard lock(display_);
    story_ = std::move(copy);
    story_title_ = title;
    StartStory();
    return "";
}

std::string StemBoard::ReplayStory() {
    DisplayLockGuard lock(display_);
    if (story_ == nullptr) {
        return "There is no story to replay yet";
    }
    StartStory();
    return "";
}

void StemBoard::StartStory() {
    Begin(story_title_, Mode::kStory);
    close_after_ms_ = kStoryPlayingCloseMs;
    const cJSON* scene;
    cJSON_ArrayForEach(scene, story_.get()) { scenes_.push_back(scene); }

    caption_ = AddLabel(canvas_, "", text_font_, text_color_);
    lv_obj_set_width(caption_, canvas_w_ - 8);
    lv_label_set_long_mode(caption_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(caption_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(caption_, background_color_, 0);
    lv_obj_set_style_bg_opa(caption_, LV_OPA_80, 0);
    lv_obj_set_style_radius(caption_, 6, 0);
    lv_obj_align(caption_, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_add_flag(caption_, LV_OBJ_FLAG_HIDDEN);

    story_synced_ = false;
    StartScene(0);
}

void StemBoard::StartScene(int index) {
    scene_ = index;
    scene_tick_ = lv_tick_get();
    pending_.clear();
    const cJSON* scene = scenes_[index];

    const char* say = StringItem(scene, "say");
    if (say != nullptr && say[0] != '\0') {
        lv_label_set_text(caption_, Truncate(say).c_str());
        lv_obj_remove_flag(caption_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(caption_, LV_OBJ_FLAG_HIDDEN);
    }

    uint32_t end = 0;
    const cJSON* action;
    cJSON_ArrayForEach(action, cJSON_GetObjectItem(scene, "actions")) {
        auto at = static_cast<uint32_t>(
            std::clamp(NumberItem(action, "at", 0), 0.0, (double)kMaxActionMs));
        auto ms = static_cast<uint32_t>(
            std::clamp(NumberItem(action, "ms", kDefaultActionMs), 0.0, (double)kMaxActionMs));
        end = std::max(end, at + ms);
        pending_.push_back({at, action});
    }
    uint32_t estimate = std::max<uint32_t>(
        kMinSceneMs, say != nullptr ? strlen(say) * kMsPerCaptionChar : 0);
    double fixed = NumberItem(scene, "ms", 0);
    scene_duration_ = fixed > 0 ? static_cast<uint32_t>(std::clamp(fixed, 500.0, 60000.0))
                                : std::max(estimate, end + 500);
    ESP_LOGI(TAG, "Scene %d/%d, %u ms", index + 1, (int)scenes_.size(),
             (unsigned)scene_duration_);
}

void StemBoard::JumpToScene(int index) {
    // Finish what the skipped scenes would have done, without animating, so later scenes
    // find their objects where they expect them.
    for (const auto& p : pending_) {
        ExecuteAction(p.action, true);
    }
    pending_.clear();
    for (int i = scene_ + 1; i < index; i++) {
        const cJSON* action;
        cJSON_ArrayForEach(action, cJSON_GetObjectItem(scenes_[i], "actions")) {
            ExecuteAction(action, true);
        }
    }
    StartScene(index);
}

void StemBoard::TickStory() {
    if (scene_ < 0) {
        return;
    }
    uint32_t elapsed = lv_tick_elaps(scene_tick_);
    for (size_t i = 0; i < pending_.size();) {
        if (pending_[i].at <= elapsed) {
            const cJSON* action = pending_[i].action;
            pending_.erase(pending_.begin() + i);
            ExecuteAction(action, false);
        } else {
            i++;
        }
    }
    // Once speech has been matched, scenes wait for their sentence; the timer is only a
    // safety net in case a sentence never comes.
    uint32_t hold = story_synced_ ? scene_duration_ * kSyncedHoldFactor : scene_duration_;
    if (!pending_.empty() || elapsed < hold) {
        return;
    }
    if (scene_ + 1 < static_cast<int>(scenes_.size())) {
        StartScene(scene_ + 1);
    } else {
        // The final picture stays up for a while
        mode_ = Mode::kNone;
        close_after_ms_ = lv_tick_elaps(shown_tick_) + kStoryEndCloseMs;
    }
}

void StemBoard::OnSentence(const std::string& text) {
    DisplayLockGuard lock(display_);
    if (mode_ != Mode::kStory || scene_ < 0) {
        return;
    }
    auto words = Words(text.c_str());
    int count = static_cast<int>(scenes_.size());
    int best = -1;
    float best_score = kSentenceMatch;
    for (int i = scene_; i < std::min(scene_ + 4, count); i++) {
        float score = Overlap(words, Words(StringItem(scenes_[i], "say")));
        if (score >= best_score && (best < 0 || score > best_score)) {
            best = i;
            best_score = score;
        }
    }
    if (best >= 0) {
        story_synced_ = true;
        if (best > scene_) {
            JumpToScene(best);
        }
    } else if (story_synced_ && scene_ + 1 < count &&
               lv_tick_elaps(scene_tick_) >= kMinSceneBeforeSkipMs) {
        // The model paraphrased; assume it moved on to the next scene
        JumpToScene(scene_ + 1);
    }
}

StemBoard::StoryObject* StemBoard::FindStoryObject(const char* id) {
    for (auto& obj : story_objects_) {
        if (obj.id == id) {
            return &obj;
        }
    }
    return nullptr;
}

void StemBoard::ExecuteAction(const cJSON* action, bool instant) {
    const char* op = StringItem(action, "do");
    const char* id = StringItem(action, "id");
    if (op == nullptr || id == nullptr) {
        return;
    }
    uint32_t ms = instant ? 0
                          : static_cast<uint32_t>(std::clamp(
                                NumberItem(action, "ms", kDefaultActionMs), 0.0,
                                (double)kMaxActionMs));
    bool repeat = !instant && BoolItem(action, "repeat", false);

    if (strcmp(op, "add") == 0) {
        if (story_objects_.size() >= kMaxStoryObjects || FindStoryObject(id) != nullptr) {
            return;
        }
        int x = static_cast<int>(NumberItem(action, "x", canvas_w_ / 2));
        int y = static_cast<int>(NumberItem(action, "y", canvas_h_ / 2));
        StoryObject obj{id, nullptr, 0, 0, false, false, 256};
        const char* icon = StringItem(action, "icon");
        const char* type = StringItem(action, "type");
        if (icon != nullptr) {
            lv_obj_t* box = lv_obj_create(canvas_);
            MakePlain(box);
            lv_obj_set_size(box, kIconBox, kIconBox);
            BuildIcon(icon, box);
            if (cJSON_IsString(cJSON_GetObjectItem(action, "color"))) {
                Recolor(box, ParseColor(cJSON_GetObjectItem(action, "color"), text_color_));
            }
            obj = {id, box, kIconBox / 2, kIconBox / 2, true, false, 256};
        } else if (type != nullptr && strcmp(type, "text") == 0) {
            const char* size = StringItem(action, "size");
            bool large = size != nullptr && strcmp(size, "large") == 0;
            lv_obj_t* label = AddCenteredLabel(
                0, 0, Truncate(StringItem(action, "text")).c_str(),
                large ? large_font_ : text_font_,
                ParseColor(cJSON_GetObjectItem(action, "color"), text_color_));
            obj = {id, label, 0, 0, true, true, 256};
        } else {
            lv_obj_t* layer = CreateLayer(canvas_);
            AddShape(action, layer);
            // A shape's anchor is its x/y, or its first point
            double ax = NumberItem(action, "x", NAN), ay = NumberItem(action, "y", NAN);
            const cJSON* points = cJSON_GetObjectItem(action, "points");
            if (!std::isfinite(ax) || !std::isfinite(ay)) {
                if (!ReadPoint(cJSON_IsArray(points) ? cJSON_GetArrayItem(points, 0)
                                                     : cJSON_GetObjectItem(action, "from"),
                               ax, ay)) {
                    ax = ay = 0;
                }
            }
            x = static_cast<int>(ax);
            y = static_cast<int>(ay);
            obj = {id, layer, x, y, false, false, 256};
        }
        lv_obj_set_pos(obj.obj, x - obj.off_x, y - obj.off_y);
        if (obj.scalable) {
            lv_obj_set_style_transform_pivot_x(obj.obj, lv_pct(50), 0);
            lv_obj_set_style_transform_pivot_y(obj.obj, lv_pct(50), 0);
            // Icons take a numeric size; text uses "small"/"large" fonts instead
            double size = icon != nullptr ? std::clamp(NumberItem(action, "size", 1), 0.3, 3.0) : 1;
            obj.scale = static_cast<int32_t>(256 * size);
            if (obj.scale != 256) {
                lv_obj_set_style_transform_scale(obj.obj, obj.scale, 0);
            }
        }
        story_objects_.push_back(obj);
        lv_obj_move_foreground(caption_);

        const char* enter = StringItem(action, "enter");
        std::string effect = enter != nullptr ? enter : "fade";
        if (instant || effect == "none") {
            return;
        }
        uint32_t enter_ms = BoolItem(action, "slow", false) ? 1500 : 600;
        int32_t px = lv_obj_get_style_x(obj.obj, LV_PART_MAIN);
        int32_t py = lv_obj_get_style_y(obj.obj, LV_PART_MAIN);
        if (effect == "grow" && obj.scalable) {
            RunAnim(obj.obj, ExecScale, 16, obj.scale, enter_ms, lv_anim_path_overshoot);
        } else if (effect == "slide_left") {
            RunAnim(obj.obj, ExecX, px - (x + kIconBox * 2), px, enter_ms, lv_anim_path_ease_out);
        } else if (effect == "slide_right") {
            RunAnim(obj.obj, ExecX, px + (canvas_w_ - x + kIconBox * 2), px, enter_ms,
                    lv_anim_path_ease_out);
        } else if (effect == "slide_top") {
            RunAnim(obj.obj, ExecY, py - (y + kIconBox * 2), py, enter_ms, lv_anim_path_ease_out);
        } else if (effect == "slide_bottom") {
            RunAnim(obj.obj, ExecY, py + (canvas_h_ - y + kIconBox * 2), py, enter_ms,
                    lv_anim_path_ease_out);
        } else {
            RunAnim(obj.obj, ExecOpa, LV_OPA_TRANSP, LV_OPA_COVER, enter_ms);
        }
        return;
    }

    StoryObject* obj = FindStoryObject(id);
    if (obj == nullptr) {
        return;
    }
    lv_obj_t* o = obj->obj;
    double x, y;

    if (strcmp(op, "move") == 0 || strcmp(op, "path") == 0) {
        lv_anim_delete(o, ExecX);
        lv_anim_delete(o, ExecY);
        lv_anim_t query;
        lv_anim_init(&query);
        lv_anim_set_var(&query, o);
        lv_anim_custom_delete(&query, ExecPathAnim);
    }

    if (strcmp(op, "move") == 0 && ReadPoint(cJSON_GetObjectItem(action, "to"), x, y)) {
        int32_t tx = static_cast<int32_t>(x) - obj->off_x;
        int32_t ty = static_cast<int32_t>(y) - obj->off_y;
        uint32_t loops = repeat ? LV_ANIM_REPEAT_INFINITE : 0;
        int32_t fx = lv_obj_get_style_x(o, LV_PART_MAIN);
        int32_t fy = lv_obj_get_style_y(o, LV_PART_MAIN);
        RunAnim(o, ExecX, fx, tx, ms, lv_anim_path_ease_in_out, loops);
        RunAnim(o, ExecY, fy, ty, ms, lv_anim_path_ease_in_out, loops);
    } else if (strcmp(op, "path") == 0) {
        auto path = std::make_unique<PathData>();
        path->off_x = obj->off_x;
        path->off_y = obj->off_y;
        path->points.push_back({lv_obj_get_style_x(o, LV_PART_MAIN) + obj->off_x,
                                lv_obj_get_style_y(o, LV_PART_MAIN) + obj->off_y});
        const cJSON* p;
        cJSON_ArrayForEach(p, cJSON_GetObjectItem(action, "points")) {
            if (path->points.size() > kMaxPathPoints || !ReadPoint(p, x, y)) {
                break;
            }
            path->points.push_back({static_cast<int32_t>(x), static_cast<int32_t>(y)});
        }
        path->lengths.push_back(0);
        for (size_t i = 1; i < path->points.size(); i++) {
            float dx = path->points[i].x - path->points[i - 1].x;
            float dy = path->points[i].y - path->points[i - 1].y;
            path->lengths.push_back(path->lengths.back() + sqrtf(dx * dx + dy * dy));
        }
        if (ms == 0 || path->points.size() < 2) {
            MoveAlong(o, *path, 1000);
            return;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_user_data(&a, path.get());
        lv_anim_set_custom_exec_cb(&a, ExecPathAnim);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, ms);
        lv_anim_set_path_cb(&a, repeat ? lv_anim_path_linear : lv_anim_path_ease_in_out);
        if (repeat) {
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        }
        lv_anim_start(&a);
        paths_.push_back(std::move(path));
    } else if (strcmp(op, "scale") == 0 && obj->scalable) {
        int32_t to = static_cast<int32_t>(256 * std::clamp(NumberItem(action, "to", 1), 0.2, 4.0));
        int32_t from = obj->scale;
        obj->scale = to;
        RunAnim(o, ExecScale, from, to, ms,
                lv_anim_path_ease_in_out, repeat ? LV_ANIM_REPEAT_INFINITE : 0,
                repeat ? ms : 0);
    } else if (strcmp(op, "fade") == 0) {
        int32_t to = static_cast<int32_t>(255 * std::clamp(NumberItem(action, "to", 1), 0.0, 1.0));
        RunAnim(o, ExecOpa, lv_obj_get_style_opa(o, LV_PART_MAIN), to, ms);
    } else if (strcmp(op, "color") == 0) {
        Recolor(o, ParseColor(cJSON_GetObjectItem(action, "to"), text_color_));
    } else if (strcmp(op, "pulse") == 0) {
        if (instant) {
            return;
        }
        auto times =
            static_cast<uint32_t>(std::clamp(NumberItem(action, "times", 2), 1.0, 20.0));
        uint32_t half = std::max<uint32_t>(150, ms / 2);
        if (repeat) {
            times = LV_ANIM_REPEAT_INFINITE;
        }
        if (obj->scalable) {
            RunAnim(o, ExecScale, obj->scale, obj->scale * 13 / 10, half, lv_anim_path_ease_in_out,
                    times, half);
        } else {
            RunAnim(o, ExecOpa, LV_OPA_COVER, LV_OPA_30, half, lv_anim_path_ease_in_out, times,
                    half);
        }
    } else if (strcmp(op, "text") == 0 && obj->is_text) {
        lv_label_set_text(o, Truncate(StringItem(action, "text")).c_str());
    } else if (strcmp(op, "remove") == 0) {
        story_objects_.erase(story_objects_.begin() + (obj - story_objects_.data()));
        if (ms == 0) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        // Hidden rather than deleted: it goes with the overlay, and no deferred delete can
        // outlive it
        lv_anim_delete(o, ExecOpa);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_exec_cb(&a, ExecOpa);
        lv_anim_set_values(&a, lv_obj_get_style_opa(o, LV_PART_MAIN), LV_OPA_TRANSP);
        lv_anim_set_duration(&a, std::min<uint32_t>(ms, 600));
        lv_anim_set_completed_cb(&a, HideWhenDone);
        lv_anim_start(&a);
    }
}

// ---------------------------------------------------------------------------------------------
// Icons

lv_obj_t* StemBoard::AddRect(int x, int y, int w, int h, lv_color_t color, bool fill,
                             int radius) {
    lv_obj_t* rect = lv_obj_create(canvas_);
    MakePlain(rect);
    lv_obj_set_pos(rect, x, y);
    lv_obj_set_size(rect, w, h);
    lv_obj_set_style_radius(rect, radius, 0);
    if (fill) {
        lv_obj_set_style_bg_color(rect, color, 0);
        lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_color(rect, color, 0);
        lv_obj_set_style_border_width(rect, 3, 0);
    }
    return rect;
}

// A filled triangle made of thick lines fanning out from the tip.
void StemBoard::FillTriangle(int tip_x, int tip_y, int x0, int x1, int base_y, lv_color_t color) {
    for (int x = x0; x <= x1; x += 2) {
        auto points = AllocPoints(2);
        points[0] = {tip_x, tip_y};
        points[1] = {x, base_y};
        AddLine(points, 2, color, 3);
    }
}

bool StemBoard::BuildIcon(const char* name, lv_obj_t* box) {
    if (const GlyphIcon* glyph = FindGlyphIcon(name)) {
        lv_color_t color = glyph->text_color ? text_color_ : lv_palette_main(glyph->palette);
        lv_obj_t* label = AddLabel(box, glyph->utf8, icon_font_, color);
        lv_obj_center(label);
        return true;
    }
    // Drawn icons use a 44 x 44 box
    lv_obj_t* saved = canvas_;
    canvas_ = box;
    auto line = [this](int x0, int y0, int x1, int y1, lv_color_t color, int width) {
        auto points = AllocPoints(2);
        points[0] = {x0, y0};
        points[1] = {x1, y1};
        AddLine(points, 2, color, width);
    };
    auto pal = [](lv_palette_t p) { return lv_palette_main(p); };
    bool found = true;

    if (strcmp(name, "flower") == 0) {
        line(22, 24, 22, 43, pal(LV_PALETTE_GREEN), 3);
        AddCircle(28, 36, 4, pal(LV_PALETTE_LIGHT_GREEN), true);
        for (int i = 0; i < 6; i++) {
            float a = i * 2 * M_PI / 6;
            AddCircle(22 + static_cast<int>(9 * cosf(a)), 15 + static_cast<int>(9 * sinf(a)), 6,
                      pal(LV_PALETTE_PINK), true);
        }
        AddCircle(22, 15, 5, pal(LV_PALETTE_AMBER), true);
    } else if (strcmp(name, "drop") == 0) {
        FillTriangle(22, 4, 12, 32, 26, pal(LV_PALETTE_BLUE));
        AddCircle(22, 29, 11, pal(LV_PALETTE_BLUE), true);
        AddCircle(18, 30, 3, lv_palette_lighten(LV_PALETTE_BLUE, 3), true);
    } else if (strcmp(name, "fire") == 0) {
        FillTriangle(22, 2, 11, 33, 30, pal(LV_PALETTE_DEEP_ORANGE));
        AddCircle(22, 31, 12, pal(LV_PALETTE_DEEP_ORANGE), true);
        FillTriangle(22, 16, 16, 28, 34, pal(LV_PALETTE_AMBER));
        AddCircle(22, 35, 7, pal(LV_PALETTE_AMBER), true);
    } else if (strcmp(name, "tree") == 0) {
        AddRect(19, 28, 6, 15, pal(LV_PALETTE_BROWN), true);
        AddCircle(14, 22, 9, pal(LV_PALETTE_GREEN), true);
        AddCircle(30, 22, 9, pal(LV_PALETTE_GREEN), true);
        AddCircle(22, 13, 11, pal(LV_PALETTE_GREEN), true);
        AddCircle(22, 24, 9, lv_palette_darken(LV_PALETTE_GREEN, 1), true);
    } else if (strcmp(name, "plant") == 0) {
        line(10, 42, 34, 42, pal(LV_PALETTE_BROWN), 4);
        line(22, 42, 22, 16, pal(LV_PALETTE_GREEN), 3);
        AddCircle(14, 22, 7, pal(LV_PALETTE_LIGHT_GREEN), true);
        AddCircle(30, 16, 7, pal(LV_PALETTE_LIGHT_GREEN), true);
        line(22, 28, 15, 23, pal(LV_PALETTE_GREEN), 2);
        line(22, 22, 29, 17, pal(LV_PALETTE_GREEN), 2);
    } else if (strcmp(name, "person") == 0) {
        AddCircle(22, 8, 6, text_color_, true);
        line(22, 14, 22, 28, text_color_, 3);
        line(10, 19, 34, 19, text_color_, 3);
        line(22, 28, 13, 42, text_color_, 3);
        line(22, 28, 31, 42, text_color_, 3);
    } else if (strcmp(name, "bulb") == 0) {
        AddCircle(22, 16, 13, pal(LV_PALETTE_YELLOW), true);
        AddRect(16, 28, 12, 10, pal(LV_PALETTE_GREY), true, 2);
        line(16, 32, 28, 32, lv_palette_darken(LV_PALETTE_GREY, 2), 1);
        line(16, 35, 28, 35, lv_palette_darken(LV_PALETTE_GREY, 2), 1);
        line(18, 16, 22, 22, pal(LV_PALETTE_ORANGE), 2);
        line(26, 16, 22, 22, pal(LV_PALETTE_ORANGE), 2);
    } else if (strcmp(name, "battery") == 0) {
        AddRect(2, 12, 36, 20, text_color_, false, 3);
        AddRect(38, 18, 5, 8, text_color_, true, 1);
        AddRect(7, 17, 18, 10, pal(LV_PALETTE_GREEN), true, 1);
    } else if (strcmp(name, "magnet") == 0) {
        line(12, 6, 12, 26, pal(LV_PALETTE_RED), 8);
        line(32, 6, 32, 26, pal(LV_PALETTE_BLUE), 8);
        lv_obj_t* arc = lv_arc_create(canvas_);
        lv_obj_remove_style_all(arc);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(arc, 28, 28);
        lv_obj_set_pos(arc, 8, 12);
        lv_arc_set_bg_angles(arc, 0, 180);
        lv_obj_set_style_arc_color(arc, pal(LV_PALETTE_GREY), LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    } else if (strcmp(name, "car") == 0) {
        AddRect(10, 8, 24, 13, pal(LV_PALETTE_RED), true, 5);
        AddRect(14, 11, 16, 8, lv_palette_lighten(LV_PALETTE_LIGHT_BLUE, 2), true, 2);
        AddRect(2, 18, 40, 14, pal(LV_PALETTE_RED), true, 5);
        AddCircle(12, 33, 6, lv_palette_darken(LV_PALETTE_GREY, 3), true);
        AddCircle(32, 33, 6, lv_palette_darken(LV_PALETTE_GREY, 3), true);
    } else if (strcmp(name, "earth") == 0) {
        AddCircle(22, 22, 20, pal(LV_PALETTE_BLUE), true);
        AddCircle(14, 15, 7, pal(LV_PALETTE_GREEN), true);
        AddCircle(28, 29, 8, pal(LV_PALETTE_GREEN), true);
        AddCircle(31, 12, 4, pal(LV_PALETTE_GREEN), true);
    } else if (strcmp(name, "cell") == 0) {
        lv_obj_t* membrane = AddCircle(22, 22, 20, pal(LV_PALETTE_LIGHT_GREEN), true);
        lv_obj_set_style_bg_opa(membrane, LV_OPA_40, 0);
        lv_obj_set_style_border_color(membrane, pal(LV_PALETTE_GREEN), 0);
        lv_obj_set_style_border_width(membrane, 2, 0);
        AddCircle(25, 19, 7, pal(LV_PALETTE_PURPLE), true);
        AddCircle(13, 28, 3, pal(LV_PALETTE_GREEN), true);
        AddCircle(30, 32, 3, pal(LV_PALETTE_GREEN), true);
    } else if (strcmp(name, "mountain") == 0) {
        FillTriangle(22, 4, 2, 42, 42, pal(LV_PALETTE_BLUE_GREY));
        FillTriangle(22, 4, 16, 28, 14, lv_color_white());
    } else if (strcmp(name, "molecule") == 0) {
        line(10, 32, 22, 16, pal(LV_PALETTE_GREY), 3);
        line(34, 32, 22, 16, pal(LV_PALETTE_GREY), 3);
        AddCircle(22, 16, 9, pal(LV_PALETTE_RED), true);
        AddCircle(10, 32, 6, lv_palette_lighten(LV_PALETTE_GREY, 2), true);
        AddCircle(34, 32, 6, lv_palette_lighten(LV_PALETTE_GREY, 2), true);
    } else if (strcmp(name, "ball") == 0) {
        AddCircle(22, 22, 14, pal(LV_PALETTE_ORANGE), true);
        AddCircle(17, 17, 4, lv_palette_lighten(LV_PALETTE_ORANGE, 3), true);
    } else {
        found = false;
    }
    canvas_ = saved;
    return found;
}
