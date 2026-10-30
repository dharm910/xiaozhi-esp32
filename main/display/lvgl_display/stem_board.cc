#include "stem_board.h"

#include <esp_log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "display.h"
#include "lvgl_theme.h"
#include "math_font.h"
#include "stem_board_internal.h"
#include "stem_expression.h"

#define TAG "StemBoard"

LV_FONT_DECLARE(font_noto_sans_basic_30_4);
LV_FONT_DECLARE(font_stem_icons_40_4);

using namespace stem_detail;

namespace {

constexpr uint32_t kFramePeriodMs = 33;
constexpr uint32_t kAfterAnswerCloseMs = 20000;
constexpr uint32_t kDrawRevealMs = 450;
constexpr uint32_t kPlotRevealMs = 1500;
constexpr uint32_t kBurstMs = 900;
constexpr size_t kMaxShapes = 40;
constexpr int kMaxLinePoints = 32;
constexpr int kMaxSteps = 12;
constexpr int kMaxSymbols = 8;
constexpr int kMaxMarks = 6;
constexpr int kMaxCurves = 3;
constexpr float kGravity = 9.81f;
constexpr int kPad = 6;

const char* const kElements[] = {
    "",          "Hydrogen (H)",  "Helium (He)",   "Lithium (Li)", "Beryllium (Be)",
    "Boron (B)", "Carbon (C)",    "Nitrogen (N)",  "Oxygen (O)",   "Fluorine (F)",
    "Neon (Ne)", "Sodium (Na)",   "Magnesium (Mg)", "Aluminium (Al)", "Silicon (Si)",
    "Phosphorus (P)", "Sulfur (S)", "Chlorine (Cl)", "Argon (Ar)", "Potassium (K)",
    "Calcium (Ca)",
};
const char* const kElementSymbols[] = {"",   "H",  "He", "Li", "Be", "B",  "C",
                                       "N",  "O",  "F",  "Ne", "Na", "Mg", "Al",
                                       "Si", "P",  "S",  "Cl", "Ar", "K",  "Ca"};

std::string FormatNumber(double v) {
    char buf[24];
    if (fabs(v) < 1e-9) {
        v = 0;
    }
    snprintf(buf, sizeof(buf), "%.3g", v);
    return buf;
}

double NiceStep(double range) {
    double raw = range / 5;
    double mag = pow(10, floor(log10(raw)));
    double n = raw / mag;
    return (n < 1.5 ? 1 : n < 3.5 ? 2 : n < 7.5 ? 5 : 10) * mag;
}

}  // namespace

StemBoard::StemBoard(Display* display) : display_(display) {
    auto lvgl_theme = theme();
    if (lvgl_theme == nullptr) {
        return;
    }
    int spacing = lvgl_theme->spacing(2);
    top_offset_ = lvgl_theme->icon_font()->font()->line_height + spacing * 2;
    title_h_ = lvgl_theme->text_font()->font()->line_height;
    canvas_w_ = display_->width();
    canvas_h_ = display_->height() - top_offset_ - title_h_;
}

StemBoard::~StemBoard() { Clear(); }

LvglTheme* StemBoard::theme() const { return static_cast<LvglTheme*>(display_->GetTheme()); }

void StemBoard::Clear() {
    DisplayLockGuard lock(display_);
    ClearLocked();
}

void StemBoard::ClearLocked() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    title_ = nullptr;
    canvas_ = nullptr;
    anim_line_ = nullptr;
    anim_points_ = nullptr;
    anim_body_ = nullptr;
    anim_marker_ = nullptr;
    reveal_objs_.clear();
    plot_segments_.clear();
    movers_.clear();
    particles_.clear();
    quiz_options_.clear();
    // Deleting root_ also deleted every LVGL animation on its objects, so path data can go
    scenes_.clear();
    scene_ = -1;
    pending_.clear();
    story_objects_.clear();
    paths_.clear();
    caption_ = nullptr;
    point_buffers_.clear();
    mode_ = Mode::kNone;
}

void StemBoard::Begin(const std::string& title, Mode mode) {
    ClearLocked();
    auto lvgl_theme = theme();
    text_color_ = lvgl_theme->text_color();
    background_color_ = lvgl_theme->background_color();
    text_font_ = WithMathSymbols(lvgl_theme->text_font()->font());
    large_font_ = WithMathSymbols(&font_noto_sans_basic_30_4);
    icon_font_ = &font_stem_icons_40_4;

    root_ = lv_obj_create(lv_screen_active());
    MakePlain(root_);
    lv_obj_set_size(root_, display_->width(), display_->height() - top_offset_);
    lv_obj_set_pos(root_, 0, top_offset_);
    lv_obj_set_style_bg_color(root_, background_color_, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

    title_ = AddLabel(root_, title.c_str(), text_font_, Accent());
    lv_obj_set_width(title_, display_->width() - kPad * 2);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title_, kPad, 0);

    canvas_ = lv_obj_create(root_);
    MakePlain(canvas_);
    lv_obj_set_pos(canvas_, 0, title_h_);
    lv_obj_set_size(canvas_, canvas_w_, canvas_h_);

    timer_ = lv_timer_create(OnTimer, kFramePeriodMs, this);
    shown_tick_ = mode_tick_ = lv_tick_get();
    close_after_ms_ = kAutoCloseMs;
    mode_ = mode;
    revealed_ = 0;
}

void StemBoard::OnTimer(lv_timer_t* timer) {
    static_cast<StemBoard*>(lv_timer_get_user_data(timer))->Tick();
}

void StemBoard::Tick() {
    if (lv_tick_elaps(shown_tick_) > close_after_ms_) {
        ClearLocked();
        return;
    }
    uint32_t elapsed = lv_tick_elaps(mode_tick_);
    switch (mode_) {
        case Mode::kReveal:
            TickReveal(elapsed);
            break;
        case Mode::kPlot:
            TickPlot(elapsed);
            break;
        case Mode::kAnimation:
            TickAnimation(elapsed);
            break;
        case Mode::kStory:
            TickStory();
            break;
        default:
            break;
    }
    TickParticles();
}

// ---------------------------------------------------------------------------------------------
// Drawing helpers

lv_color_t StemBoard::ParseColor(const cJSON* item, lv_color_t fallback) const {
    if (!cJSON_IsString(item)) {
        return fallback;
    }
    struct {
        const char* name;
        lv_palette_t palette;
    } static const kColors[] = {
        {"red", LV_PALETTE_RED},       {"green", LV_PALETTE_GREEN},   {"blue", LV_PALETTE_BLUE},
        {"orange", LV_PALETTE_ORANGE}, {"yellow", LV_PALETTE_YELLOW}, {"purple", LV_PALETTE_PURPLE},
        {"cyan", LV_PALETTE_CYAN},     {"pink", LV_PALETTE_PINK},     {"gray", LV_PALETTE_GREY},
        {"grey", LV_PALETTE_GREY},     {"brown", LV_PALETTE_BROWN},   {"teal", LV_PALETTE_TEAL},
    };
    for (const auto& c : kColors) {
        if (strcasecmp(item->valuestring, c.name) == 0) {
            return lv_palette_main(c.palette);
        }
    }
    // "white", "black" and anything unknown use the theme's text color so it stays visible
    return fallback;
}

lv_point_precise_t* StemBoard::AllocPoints(int count) {
    point_buffers_.emplace_back(new lv_point_precise_t[count]);
    return point_buffers_.back().get();
}

lv_obj_t* StemBoard::AddLine(lv_point_precise_t* points, int count, lv_color_t color, int width,
                             bool dashed) {
    lv_obj_t* line = lv_line_create(canvas_);
    lv_line_set_points_mutable(line, points, count);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    if (dashed) {
        lv_obj_set_style_line_dash_width(line, 6, 0);
        lv_obj_set_style_line_dash_gap(line, 4, 0);
    }
    return line;
}

lv_obj_t* StemBoard::AddCircle(int cx, int cy, int r, lv_color_t color, bool fill, int border) {
    lv_obj_t* circle = lv_obj_create(canvas_);
    MakePlain(circle);
    lv_obj_set_size(circle, r * 2, r * 2);
    lv_obj_set_pos(circle, cx - r, cy - r);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    if (fill) {
        lv_obj_set_style_bg_color(circle, color, 0);
        lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_color(circle, color, 0);
        lv_obj_set_style_border_width(circle, border, 0);
    }
    return circle;
}

lv_obj_t* StemBoard::AddLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                              lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

lv_obj_t* StemBoard::AddCenteredLabel(int cx, int cy, const char* text, const lv_font_t* font,
                                      lv_color_t color) {
    lv_obj_t* label = AddLabel(canvas_, text, font, color);
    lv_obj_set_style_max_width(label, canvas_w_, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(label, cx, cy);
    lv_obj_set_style_translate_x(label, lv_pct(-50), 0);
    lv_obj_set_style_translate_y(label, lv_pct(-50), 0);
    return label;
}

lv_obj_t* StemBoard::AddArrowHead(int x0, int y0, int x1, int y1, lv_color_t color, int width) {
    constexpr float kLength = 10.0f;
    constexpr float kSpread = 0.45f;  // radians
    float dir = atan2f(y1 - y0, x1 - x0);
    auto points = AllocPoints(3);
    points[0] = {static_cast<int32_t>(x1 - kLength * cosf(dir - kSpread)),
                 static_cast<int32_t>(y1 - kLength * sinf(dir - kSpread))};
    points[1] = {x1, y1};
    points[2] = {static_cast<int32_t>(x1 - kLength * cosf(dir + kSpread)),
                 static_cast<int32_t>(y1 - kLength * sinf(dir + kSpread))};
    return AddLine(points, 3, color, width);
}

// ---------------------------------------------------------------------------------------------
// Diagrams

std::string StemBoard::AddShape(const cJSON* shape, lv_obj_t* layer) {
    const char* type = StringItem(shape, "type");
    if (type == nullptr) {
        return "Each shape needs a string 'type'";
    }
    lv_color_t color = ParseColor(cJSON_GetObjectItem(shape, "color"), text_color_);
    bool fill = BoolItem(shape, "fill", false);
    int width = std::clamp(static_cast<int>(NumberItem(shape, "width", 2)), 1, 8);
    auto cx = [this](double v) {
        return static_cast<int32_t>(std::clamp(v, 0.0, (double)canvas_w_));
    };
    auto cy = [this](double v) {
        return static_cast<int32_t>(std::clamp(v, 0.0, (double)canvas_h_));
    };

    // Shapes are created on canvas_ and then moved into their own layer so they can be revealed
    // one at a time.
    lv_obj_t* saved_canvas = canvas_;
    canvas_ = layer;
    std::string error;

    if (strcmp(type, "line") == 0 || strcmp(type, "polygon") == 0 || strcmp(type, "arrow") == 0) {
        bool closed = strcmp(type, "polygon") == 0 || BoolItem(shape, "closed", false);
        bool arrow = strcmp(type, "arrow") == 0 || BoolItem(shape, "arrow", false);
        const cJSON* pts = cJSON_GetObjectItem(shape, "points");
        std::vector<lv_point_precise_t> list;
        double x, y;
        if (cJSON_IsArray(pts)) {
            const cJSON* p;
            cJSON_ArrayForEach(p, pts) {
                if (list.size() >= kMaxLinePoints || !ReadPoint(p, x, y)) {
                    break;
                }
                list.push_back({cx(x), cy(y)});
            }
        } else if (ReadPoint(cJSON_GetObjectItem(shape, "from"), x, y)) {
            list.push_back({cx(x), cy(y)});
            if (ReadPoint(cJSON_GetObjectItem(shape, "to"), x, y)) {
                list.push_back({cx(x), cy(y)});
            }
        }
        if (list.size() < 2) {
            error = std::string("'") + type + "' needs 'points' [[x,y],[x,y],...] or 'from'/'to'";
        } else {
            if (closed) {
                list.push_back(list.front());
            }
            auto points = AllocPoints(list.size());
            std::copy(list.begin(), list.end(), points);
            AddLine(points, list.size(), color, width, BoolItem(shape, "dashed", false));
            if (arrow && !closed) {
                auto& a = list[list.size() - 2];
                auto& b = list.back();
                AddArrowHead(a.x, a.y, b.x, b.y, color, width);
            }
        }
    } else if (strcmp(type, "rect") == 0) {
        int x = cx(NumberItem(shape, "x", 0));
        int y = cy(NumberItem(shape, "y", 0));
        int w = std::clamp(static_cast<int>(NumberItem(shape, "w", 20)), 1, canvas_w_ - x);
        int h = std::clamp(static_cast<int>(NumberItem(shape, "h", 20)), 1, canvas_h_ - y);
        lv_obj_t* rect = lv_obj_create(canvas_);
        MakePlain(rect);
        lv_obj_set_pos(rect, x, y);
        lv_obj_set_size(rect, w, h);
        lv_obj_set_style_radius(rect, std::clamp((int)NumberItem(shape, "radius", 0), 0, 40), 0);
        if (fill) {
            lv_obj_set_style_bg_color(rect, color, 0);
            lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_border_color(rect, color, 0);
            lv_obj_set_style_border_width(rect, width, 0);
        }
    } else if (strcmp(type, "circle") == 0 || strcmp(type, "dot") == 0) {
        bool dot = strcmp(type, "dot") == 0;
        int r = std::clamp(static_cast<int>(NumberItem(shape, "r", dot ? 4 : 20)), 1, canvas_w_);
        AddCircle(cx(NumberItem(shape, "x", 0)), cy(NumberItem(shape, "y", 0)), r, color,
                  dot || fill, width);
    } else if (strcmp(type, "arc") == 0) {
        int r = std::clamp(static_cast<int>(NumberItem(shape, "r", 20)), 2, canvas_w_);
        int x = cx(NumberItem(shape, "x", 0));
        int y = cy(NumberItem(shape, "y", 0));
        // Input angles are math style: degrees counter-clockwise from the +x axis.
        // LVGL measures clockwise, so the arc runs from -end to -start.
        int start = static_cast<int>(NumberItem(shape, "start", 0));
        int end = static_cast<int>(NumberItem(shape, "end", 90));
        lv_obj_t* arc = lv_arc_create(canvas_);
        lv_obj_remove_style_all(arc);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(arc, r * 2, r * 2);
        lv_obj_set_pos(arc, x - r, y - r);
        auto wrap = [](int angle) { return ((angle % 360) + 360) % 360; };
        lv_arc_set_bg_angles(arc, wrap(360 - end), wrap(360 - start));
        lv_obj_set_style_arc_color(arc, color, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    } else if (strcmp(type, "text") == 0) {
        const char* text = StringItem(shape, "text");
        if (text == nullptr) {
            error = "'text' shape needs a 'text' string";
        } else {
            const char* size = StringItem(shape, "size");
            bool large = size != nullptr && strcmp(size, "large") == 0;
            AddCenteredLabel(cx(NumberItem(shape, "x", 0)), cy(NumberItem(shape, "y", 0)),
                             Truncate(text).c_str(), large ? large_font_ : text_font_, color);
        }
    } else {
        error = std::string("Unknown shape type '") + type +
                "'. Use line, polygon, arrow, rect, circle, dot, arc or text";
    }
    canvas_ = saved_canvas;
    return error;
}

std::string StemBoard::Draw(const std::string& title, const cJSON* shapes, bool animate) {
    if (!cJSON_IsArray(shapes) || cJSON_GetArraySize(shapes) == 0) {
        return "'shapes' must be a non-empty JSON array";
    }
    DisplayLockGuard lock(display_);
    Begin(title, Mode::kReveal);
    reveal_interval_ms_ = animate ? kDrawRevealMs : 0;
    reveal_highlight_ = false;
    const cJSON* shape;
    cJSON_ArrayForEach(shape, shapes) {
        if (reveal_objs_.size() >= kMaxShapes) {
            break;
        }
        lv_obj_t* layer = CreateLayer(canvas_);
        auto error = AddShape(shape, layer);
        if (!error.empty()) {
            ClearLocked();
            return error;
        }
        if (animate) {
            lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
        }
        reveal_objs_.push_back(layer);
    }
    revealed_ = animate ? 0 : reveal_objs_.size();
    return "";
}

void StemBoard::TickReveal(uint32_t elapsed) {
    if (revealed_ >= reveal_objs_.size()) {
        return;
    }
    // The first item appears immediately, the rest one per interval.
    size_t target = reveal_objs_.size();
    if (reveal_interval_ms_ > 0) {
        target = std::min(target, (size_t)(elapsed / reveal_interval_ms_ + 1));
    }
    while (revealed_ < target) {
        lv_obj_t* obj = reveal_objs_[revealed_];
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        if (reveal_highlight_) {
            // USER_1 marks rows with their own background (wrong steps), which stay as they are
            if (revealed_ > 0 && !lv_obj_has_state(reveal_objs_[revealed_ - 1], LV_STATE_USER_1)) {
                lv_obj_set_style_bg_opa(reveal_objs_[revealed_ - 1], LV_OPA_TRANSP, 0);
            }
            if (!lv_obj_has_state(obj, LV_STATE_USER_1)) {
                lv_obj_set_style_bg_color(obj, Accent(), 0);
                lv_obj_set_style_bg_opa(obj, LV_OPA_30, 0);
            }
            lv_obj_scroll_to_view(obj, LV_ANIM_ON);
        }
        revealed_++;
    }
}

// ---------------------------------------------------------------------------------------------
// Graphs

std::string StemBoard::Plot(const std::string& title, const std::string& expressions,
                            double x_min, double x_max, const std::string& y_range,
                            const cJSON* marks) {
    if (!(x_max > x_min) || !std::isfinite(x_min) || !std::isfinite(x_max)) {
        return "x_max must be greater than x_min";
    }
    std::vector<std::string> sources;
    size_t start = 0;
    while (start <= expressions.size() && sources.size() < kMaxCurves) {
        size_t end = expressions.find(';', start);
        std::string part = expressions.substr(start, end == std::string::npos ? std::string::npos
                                                                               : end - start);
        // Accept "y = ..." and "f(x) = ..." prefixes
        size_t eq = part.find('=');
        if (eq != std::string::npos) {
            part = part.substr(eq + 1);
        }
        size_t first = part.find_first_not_of(" \t");
        if (first != std::string::npos) {
            sources.push_back(part.substr(first, part.find_last_not_of(" \t") - first + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (sources.empty()) {
        return "No expression given";
    }
    std::vector<StemExpression> curves(sources.size());
    for (size_t i = 0; i < sources.size(); i++) {
        auto error = curves[i].Compile(sources[i]);
        if (!error.empty()) {
            return "Expression '" + sources[i] + "': " + error;
        }
    }

    const int w = canvas_w_;
    const int h = canvas_h_;
    const int samples = std::max(2, w / 2);
    std::vector<std::vector<double>> values(curves.size(), std::vector<double>(samples));
    std::vector<double> finite;
    for (size_t c = 0; c < curves.size(); c++) {
        for (int i = 0; i < samples; i++) {
            double x = x_min + (x_max - x_min) * i / (samples - 1);
            values[c][i] = curves[c].Evaluate(x);
            if (std::isfinite(values[c][i])) {
                finite.push_back(values[c][i]);
            }
        }
    }

    double y_min, y_max;
    if (y_range.empty() || y_range == "auto") {
        if (finite.empty()) {
            return "The expression is undefined over the whole x range";
        }
        std::sort(finite.begin(), finite.end());
        y_min = finite.front();
        y_max = finite.back();
        // Asymptotes (tan, 1/x) would flatten everything else; fall back to the middle 90%.
        double p5 = finite[finite.size() * 5 / 100];
        double p95 = finite[finite.size() * 95 / 100];
        if (y_max - y_min > 20 * (p95 - p5) && p95 > p5) {
            y_min = p5 - (p95 - p5) * 0.3;
            y_max = p95 + (p95 - p5) * 0.3;
        }
        if (y_max - y_min < 1e-9) {
            y_min -= 1;
            y_max += 1;
        }
        double padding = (y_max - y_min) * 0.08;
        y_min -= padding;
        y_max += padding;
    } else if (sscanf(y_range.c_str(), "%lf , %lf", &y_min, &y_max) != 2 || !(y_max > y_min)) {
        return "y_range must be \"auto\" or \"min,max\" with max > min";
    }

    DisplayLockGuard lock(display_);
    Begin(title.empty() ? (sources.size() == 1 ? "y = " + sources[0] : "Graphs") : title,
          Mode::kPlot);

    auto px = [&](double x) {
        return static_cast<int32_t>((x - x_min) / (x_max - x_min) * (w - 1));
    };
    auto py = [&](double y) {
        return static_cast<int32_t>((y_max - y) / (y_max - y_min) * (h - 1));
    };

    // Grid
    lv_color_t grid_color = Muted();
    double x_step = NiceStep(x_max - x_min);
    double y_step = NiceStep(y_max - y_min);
    for (double gx = ceil(x_min / x_step) * x_step; gx <= x_max; gx += x_step) {
        auto points = AllocPoints(2);
        points[0] = {px(gx), 0};
        points[1] = {px(gx), h - 1};
        lv_obj_set_style_line_opa(AddLine(points, 2, grid_color, 1), LV_OPA_30, 0);
    }
    for (double gy = ceil(y_min / y_step) * y_step; gy <= y_max; gy += y_step) {
        auto points = AllocPoints(2);
        points[0] = {0, py(gy)};
        points[1] = {w - 1, py(gy)};
        lv_obj_set_style_line_opa(AddLine(points, 2, grid_color, 1), LV_OPA_30, 0);
    }
    // Axes
    if (x_min <= 0 && x_max >= 0) {
        auto points = AllocPoints(2);
        points[0] = {px(0), 0};
        points[1] = {px(0), h - 1};
        AddLine(points, 2, text_color_, 1);
    }
    if (y_min <= 0 && y_max >= 0) {
        auto points = AllocPoints(2);
        points[0] = {0, py(0)};
        points[1] = {w - 1, py(0)};
        AddLine(points, 2, text_color_, 1);
    }

    // Range labels in the corners
    auto corner_label = [&](const std::string& text, lv_align_t align, int dy) {
        lv_obj_t* label = AddLabel(canvas_, text.c_str(), text_font_, grid_color);
        lv_obj_set_style_bg_color(label, background_color_, 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
        lv_obj_align(label, align, align == LV_ALIGN_BOTTOM_RIGHT ? -2 : 2, dy);
    };
    corner_label(FormatNumber(y_max), LV_ALIGN_TOP_LEFT, 0);
    corner_label(FormatNumber(y_min), LV_ALIGN_BOTTOM_LEFT, -title_h_);
    corner_label(FormatNumber(x_min), LV_ALIGN_BOTTOM_LEFT, 0);
    corner_label(FormatNumber(x_max), LV_ALIGN_BOTTOM_RIGHT, 0);

    // Curves, split wherever the function leaves the view or jumps (asymptotes)
    static const lv_palette_t kCurveColors[] = {LV_PALETTE_BLUE, LV_PALETTE_ORANGE,
                                                LV_PALETTE_GREEN};
    for (size_t c = 0; c < curves.size(); c++) {
        lv_color_t color = lv_palette_main(kCurveColors[c]);
        std::vector<lv_point_precise_t> segment;
        int first_sample = 0;
        auto flush = [&]() {
            if (segment.size() >= 2) {
                auto points = AllocPoints(segment.size());
                std::copy(segment.begin(), segment.end(), points);
                lv_obj_t* line = AddLine(points, segment.size(), color, 2);
                lv_line_set_points_mutable(line, points, 0);
                plot_segments_.push_back({line, points, (int)segment.size(), first_sample});
            }
            segment.clear();
        };
        auto clamped = [&](int i) -> lv_point_precise_t {
            double x = x_min + (x_max - x_min) * i / (samples - 1);
            double y = std::clamp(values[c][i], y_min, y_max);
            return {px(x), py(y)};
        };
        for (int i = 0; i < samples; i++) {
            double v = values[c][i];
            bool visible = std::isfinite(v) && v >= y_min && v <= y_max;
            if (visible) {
                lv_point_precise_t p = clamped(i);
                if (!segment.empty() && abs(p.y - segment.back().y) > h * 4 / 5) {
                    flush();
                }
                if (segment.empty()) {
                    first_sample = i;
                    if (i > 0 && std::isfinite(values[c][i - 1])) {
                        segment.push_back(clamped(i - 1));
                    }
                }
                segment.push_back(p);
            } else {
                if (!segment.empty() && std::isfinite(v)) {
                    segment.push_back(clamped(i));
                }
                flush();
            }
        }
        flush();

        if (curves.size() > 1) {
            std::string legend = "y = " + sources[c];
            lv_obj_t* label = AddLabel(canvas_, legend.c_str(), text_font_, color);
            lv_obj_set_style_bg_color(label, background_color_, 0);
            lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
            lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -2, c * text_font_->line_height);
        }
    }
    plot_samples_ = samples;

    // Marked points, e.g. roots or a vertex
    if (cJSON_IsArray(marks)) {
        int count = 0;
        const cJSON* mark;
        cJSON_ArrayForEach(mark, marks) {
            if (count++ >= kMaxMarks) {
                break;
            }
            double mx = NumberItem(mark, "x", NAN);
            double my = NumberItem(mark, "y", NAN);
            if (!std::isfinite(mx) || !std::isfinite(my) || mx < x_min || mx > x_max ||
                my < y_min || my > y_max) {
                continue;
            }
            AddCircle(px(mx), py(my), 4, Failure(), true);
            const char* text = StringItem(mark, "label");
            std::string label = text != nullptr
                                    ? Truncate(text)
                                    : "(" + FormatNumber(mx) + ", " + FormatNumber(my) + ")";
            int ly = py(my) - text_font_->line_height / 2 - 4;
            if (ly < text_font_->line_height / 2) {
                ly = py(my) + text_font_->line_height / 2 + 4;
            }
            lv_obj_t* l = AddCenteredLabel(px(mx), ly, label.c_str(), text_font_, Failure());
            lv_obj_set_style_bg_color(l, background_color_, 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_70, 0);
        }
    }
    return "";
}

void StemBoard::TickPlot(uint32_t elapsed) {
    int shown = std::min<int>(plot_samples_, plot_samples_ * elapsed / kPlotRevealMs + 1);
    for (auto& seg : plot_segments_) {
        // +1 accounts for the clamped edge point a segment may start with
        int visible = std::clamp(shown - seg.first_sample + 1, 0, seg.count);
        lv_line_set_points_mutable(seg.line, seg.points, visible >= 2 ? visible : 0);
    }
    if (shown >= plot_samples_) {
        mode_ = Mode::kNone;
    }
}

// ---------------------------------------------------------------------------------------------
// Step-by-step solutions

std::string StemBoard::ShowSteps(const std::string& title, const cJSON* steps, int interval_ms) {
    if (!cJSON_IsArray(steps) || cJSON_GetArraySize(steps) == 0) {
        return "'steps' must be a non-empty JSON array";
    }
    DisplayLockGuard lock(display_);
    Begin(title, Mode::kReveal);
    reveal_interval_ms_ = interval_ms;
    reveal_highlight_ = interval_ms > 0;

    lv_obj_t* list = lv_obj_create(canvas_);
    MakePlain(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_style_pad_hor(list, kPad / 2, 0);

    int number = 0;
    const cJSON* step;
    cJSON_ArrayForEach(step, steps) {
        if (number >= kMaxSteps) {
            break;
        }
        number++;
        const char* text = cJSON_IsString(step) ? step->valuestring : StringItem(step, "text");
        if (text == nullptr) {
            ClearLocked();
            return "Each step must be a string or an object with a 'text' string";
        }
        const char* note = StringItem(step, "note");
        const char* status = StringItem(step, "status");
        bool wrong = status != nullptr && strcmp(status, "wrong") == 0;
        bool ok = status != nullptr && strcmp(status, "ok") == 0;

        lv_obj_t* row = lv_obj_create(list);
        MakePlain(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 3, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        if (wrong) {
            lv_obj_add_state(row, LV_STATE_USER_1);
            lv_obj_set_style_bg_color(row, Failure(), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
        }

        char marker[16];
        snprintf(marker, sizeof(marker), "%d.", number);
        lv_obj_t* icon = AddLabel(row, wrong ? "✗" : ok ? "✓" : marker, text_font_,
                                  wrong ? Failure() : ok ? Success() : Muted());
        lv_obj_set_width(icon, text_font_->line_height + 4);

        lv_obj_t* body = lv_obj_create(row);
        MakePlain(body);
        lv_obj_set_flex_grow(body, 1);
        lv_obj_set_height(body, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

        lv_obj_t* label = AddLabel(body, Truncate(text).c_str(), text_font_, text_color_);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        if (note != nullptr && note[0] != '\0') {
            lv_obj_t* note_label = AddLabel(body, Truncate(note).c_str(), text_font_,
                                            wrong ? Failure() : Muted());
            lv_obj_set_width(note_label, lv_pct(100));
            lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
        }
        if (interval_ms > 0) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
        reveal_objs_.push_back(row);
    }
    revealed_ = interval_ms > 0 ? 0 : reveal_objs_.size();
    return "";
}

// ---------------------------------------------------------------------------------------------
// Quiz

std::string StemBoard::AskQuiz(const std::string& question, const cJSON* options) {
    int count = cJSON_IsArray(options) ? cJSON_GetArraySize(options) : 0;
    if (count < 2 || count > 4) {
        return "'options' must be a JSON array of 2 to 4 strings";
    }
    DisplayLockGuard lock(display_);
    Begin("", Mode::kQuiz);
    quiz_answered_ = false;
    UpdateQuizTitle();

    lv_obj_t* list = lv_obj_create(canvas_);
    MakePlain(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_style_pad_hor(list, kPad, 0);

    lv_obj_t* q = AddLabel(list, Truncate(question.c_str()).c_str(), text_font_, text_color_);
    lv_obj_set_width(q, lv_pct(100));
    lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);

    int index = 0;
    const cJSON* option;
    cJSON_ArrayForEach(option, options) {
        if (!cJSON_IsString(option)) {
            ClearLocked();
            return "Each option must be a string";
        }
        lv_obj_t* row = lv_obj_create(list);
        MakePlain(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, Muted(), 0);
        lv_obj_set_style_pad_hor(row, 4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

        char letter[2] = {static_cast<char>('A' + index), '\0'};
        lv_obj_t* letter_label = AddLabel(row, letter, text_font_, Accent());
        lv_obj_set_width(letter_label, text_font_->line_height);
        lv_obj_t* text = AddLabel(row, Truncate(option->valuestring).c_str(), text_font_,
                                  text_color_);
        lv_obj_set_flex_grow(text, 1);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        quiz_options_.push_back(row);
        index++;
    }
    return "";
}

void StemBoard::UpdateQuizTitle() {
    char title[64];
    if (quiz_total_ == 0) {
        snprintf(title, sizeof(title), "Quiz");
    } else if (quiz_streak_ >= 2) {
        snprintf(title, sizeof(title), "Quiz · Score %d/%d · ★ %d in a row", quiz_correct_,
                 quiz_total_, quiz_streak_);
    } else {
        snprintf(title, sizeof(title), "Quiz · Score %d/%d", quiz_correct_, quiz_total_);
    }
    lv_label_set_text(title_, title);
}

std::string StemBoard::AnswerQuiz(const std::string& correct_option,
                                  const std::string& student_answer, std::string& result) {
    DisplayLockGuard lock(display_);
    if (mode_ != Mode::kQuiz || quiz_options_.empty()) {
        return "No quiz question is on screen; call self.stem.quiz_ask first";
    }
    if (quiz_answered_) {
        return "This question was already answered; ask a new question";
    }
    auto to_index = [this](const std::string& s) {
        if (s.size() != 1) {
            return -1;
        }
        int i = toupper(static_cast<unsigned char>(s[0])) - 'A';
        return i >= 0 && i < (int)quiz_options_.size() ? i : -1;
    };
    int correct = to_index(correct_option);
    if (correct < 0) {
        return "correct_option must be one of the shown letters (A, B, ...)";
    }
    int chosen = to_index(student_answer);
    bool right = chosen == correct;

    quiz_answered_ = true;
    quiz_total_++;
    if (right) {
        quiz_correct_++;
        quiz_streak_++;
    } else {
        quiz_streak_ = 0;
    }
    UpdateQuizTitle();

    lv_obj_set_style_bg_color(quiz_options_[correct], Success(), 0);
    lv_obj_set_style_bg_opa(quiz_options_[correct], LV_OPA_40, 0);
    lv_obj_set_style_border_color(quiz_options_[correct], Success(), 0);
    if (chosen >= 0 && !right) {
        lv_obj_set_style_bg_color(quiz_options_[chosen], Failure(), 0);
        lv_obj_set_style_bg_opa(quiz_options_[chosen], LV_OPA_40, 0);
        lv_obj_set_style_border_color(quiz_options_[chosen], Failure(), 0);
    }

    char verdict[32];
    if (right) {
        snprintf(verdict, sizeof(verdict), "✓ Correct!");
    } else {
        snprintf(verdict, sizeof(verdict), "✗ Answer: %c", 'A' + correct);
    }
    lv_obj_t* banner = AddCenteredLabel(canvas_w_ / 2, canvas_h_ / 2, verdict, large_font_,
                                        right ? Success() : Failure());
    lv_obj_set_style_bg_color(banner, background_color_, 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_80, 0);
    lv_obj_set_style_radius(banner, 8, 0);
    lv_obj_set_style_pad_hor(banner, 10, 0);
    if (right) {
        StartBurst(Success());
    }
    close_after_ms_ = lv_tick_elaps(shown_tick_) + kAfterAnswerCloseMs;
    result = std::string(right ? "correct" : "incorrect") + ", score " +
             std::to_string(quiz_correct_) + "/" + std::to_string(quiz_total_) + ", streak " +
             std::to_string(quiz_streak_);
    return "";
}

void StemBoard::StartBurst(lv_color_t color) {
    static const lv_palette_t kBurstColors[] = {LV_PALETTE_YELLOW, LV_PALETTE_ORANGE,
                                                LV_PALETTE_PINK, LV_PALETTE_CYAN};
    constexpr int kCount = 16;
    for (int i = 0; i < kCount; i++) {
        float angle = 2 * M_PI * i / kCount;
        float speed = (i % 2 ? 150.0f : 100.0f);
        lv_color_t c = i % 3 == 0 ? color : lv_palette_main(kBurstColors[i % 4]);
        lv_obj_t* dot = AddCircle(canvas_w_ / 2, canvas_h_ / 2, 3 + i % 3, c, true);
        particles_.push_back({dot, speed * cosf(angle), speed * sinf(angle) - 60});
    }
    particles_tick_ = lv_tick_get();
}

void StemBoard::TickParticles() {
    if (particles_.empty()) {
        return;
    }
    uint32_t elapsed = lv_tick_elaps(particles_tick_);
    if (elapsed > kBurstMs) {
        for (auto& p : particles_) {
            lv_obj_delete(p.obj);
        }
        particles_.clear();
        return;
    }
    float t = elapsed / 1000.0f;
    for (auto& p : particles_) {
        int x = canvas_w_ / 2 + static_cast<int>(p.vx * t);
        int y = canvas_h_ / 2 + static_cast<int>(p.vy * t + 200 * t * t);
        lv_obj_set_pos(p.obj, x, y);
        lv_obj_set_style_opa(p.obj, LV_OPA_COVER - LV_OPA_COVER * elapsed / kBurstMs, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// Simulations

std::string StemBoard::Animate(const AnimationParams& params) {
    static const struct {
        const char* name;
        Animation animation;
    } kAnimations[] = {
        {"pendulum", Animation::kPendulum}, {"projectile", Animation::kProjectile},
        {"wave", Animation::kWave},         {"orbit", Animation::kOrbit},
        {"atom", Animation::kAtom},
    };
    bool found = false;
    Animation animation = Animation::kPendulum;
    for (const auto& a : kAnimations) {
        if (params.name == a.name) {
            animation = a.animation;
            found = true;
        }
    }
    if (!found) {
        return "Unknown animation '" + params.name +
               "'. Use pendulum, projectile, wave, orbit or atom";
    }

    DisplayLockGuard lock(display_);
    Begin("", Mode::kAnimation);
    animation_ = animation;
    animation_params_ = params;
    const int w = canvas_w_;
    const int h = canvas_h_;
    char info[96];

    auto info_label = [&](lv_align_t align) {
        lv_obj_t* label = AddLabel(canvas_, info, text_font_, Muted());
        lv_obj_align(label, align, align == LV_ALIGN_TOP_RIGHT ? -kPad : kPad, 0);
    };

    switch (animation) {
        case Animation::kPendulum: {
            auto& p = animation_params_;
            p.angle = p.angle > 0 ? std::clamp(p.angle, 1, 60) : 25;
            p.length_cm = std::clamp(p.length_cm, 10, 1000);
            double length = p.length_cm / 100.0;
            double period = 2 * M_PI * sqrt(length / kGravity);
            lv_label_set_text(title_, "Simple pendulum");
            snprintf(info, sizeof(info), "L = %.2f m\nT = 2π√(L/g)\n   = %.2f s", length, period);
            info_label(LV_ALIGN_TOP_LEFT);
            auto rest = AllocPoints(2);
            rest[0] = {w / 2, 6};
            rest[1] = {w / 2, h - 4};
            lv_obj_set_style_line_opa(AddLine(rest, 2, Muted(), 1, true), LV_OPA_50, 0);
            auto pivot = AllocPoints(2);
            pivot[0] = {w / 2 - 20, 6};
            pivot[1] = {w / 2 + 20, 6};
            AddLine(pivot, 2, text_color_, 3);
            anim_points_ = AllocPoints(2);
            anim_point_capacity_ = 2;
            anim_points_[0] = {w / 2, 6};
            anim_points_[1] = {w / 2, h - 20};
            anim_line_ = AddLine(anim_points_, 2, text_color_, 2);
            anim_body_ = AddCircle(w / 2, h - 20, 11, lv_palette_main(LV_PALETTE_ORANGE), true);
            break;
        }
        case Animation::kProjectile: {
            auto& p = animation_params_;
            p.angle = p.angle > 0 ? std::clamp(p.angle, 5, 85) : 45;
            p.speed = std::clamp(p.speed, 1, 100);
            double theta = p.angle * M_PI / 180;
            double flight = 2 * p.speed * sin(theta) / kGravity;
            double range = p.speed * cos(theta) * flight;
            double height = pow(p.speed * sin(theta), 2) / (2 * kGravity);
            lv_label_set_text(title_, "Projectile motion");
            snprintf(info, sizeof(info), "v = %d m/s, θ = %d°\nR %.1f m · H %.1f m · t %.1f s",
                     p.speed, p.angle, range, height, flight);
            info_label(LV_ALIGN_TOP_LEFT);
            auto ground = AllocPoints(2);
            ground[0] = {0, h - 6};
            ground[1] = {w - 1, h - 6};
            AddLine(ground, 2, Muted(), 2);
            // Launch velocity arrow
            int ax = 12 + static_cast<int>(40 * cos(theta));
            int ay = h - 6 - static_cast<int>(40 * sin(theta));
            auto arrow = AllocPoints(2);
            arrow[0] = {12, h - 6};
            arrow[1] = {ax, ay};
            AddLine(arrow, 2, Success(), 2);
            AddArrowHead(12, h - 6, ax, ay, Success(), 2);
            anim_point_capacity_ = 64;
            anim_points_ = AllocPoints(anim_point_capacity_);
            anim_line_ = AddLine(anim_points_, 0, Accent(), 2, true);
            anim_body_ = AddCircle(12, h - 6, 6, lv_palette_main(LV_PALETTE_ORANGE), true);
            break;
        }
        case Animation::kWave: {
            auto& p = animation_params_;
            p.count = std::clamp(p.count, 1, 5);
            lv_label_set_text(title_, "Travelling wave");
            snprintf(info, sizeof(info), "y = A·sin(kx − ωt)");
            lv_obj_t* label = AddLabel(canvas_, info, text_font_, Muted());
            lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, kPad, 0);
            auto axis = AllocPoints(2);
            axis[0] = {0, (h - title_h_) / 2};
            axis[1] = {w - 1, (h - title_h_) / 2};
            lv_obj_set_style_line_opa(AddLine(axis, 2, Muted(), 1, true), LV_OPA_60, 0);
            anim_point_capacity_ = 81;
            anim_points_ = AllocPoints(anim_point_capacity_);
            anim_line_ = AddLine(anim_points_, 0, Accent(), 3);
            anim_marker_ = AddCircle(w / 4, (h - title_h_) / 2, 6, Failure(), true);
            break;
        }
        case Animation::kOrbit: {
            auto& p = animation_params_;
            p.count = std::clamp(p.count, 1, 6);
            lv_label_set_text(title_, "Planetary orbits");
            snprintf(info, sizeof(info), "T² ∝ r³");
            info_label(LV_ALIGN_TOP_LEFT);
            static const lv_palette_t kPlanetColors[] = {LV_PALETTE_GREY, LV_PALETTE_ORANGE,
                                                         LV_PALETTE_BLUE, LV_PALETTE_RED,
                                                         LV_PALETTE_DEEP_ORANGE, LV_PALETTE_AMBER};
            int r_max = std::min(w, h) / 2 - 8;
            AddCircle(w / 2, h / 2, 9, lv_palette_main(LV_PALETTE_YELLOW), true);
            for (int i = 0; i < p.count; i++) {
                float r = p.count == 1 ? r_max * 0.7f : 20 + (r_max - 20) * i / (p.count - 1.0f);
                lv_obj_t* ring = AddCircle(w / 2, h / 2, static_cast<int>(r), Muted(), false, 1);
                lv_obj_set_style_border_opa(ring, LV_OPA_50, 0);
                int size = 6 + (i % 3) * 2;
                lv_obj_t* planet =
                    AddCircle(0, 0, size / 2, lv_palette_main(kPlanetColors[i]), true);
                // Kepler's third law: angular speed falls as r^-1.5
                float speed = (2 * M_PI / 3.0f) * powf(20.0f / r, 1.5f);
                movers_.push_back({planet, r, i * 1.3f, speed, size});
            }
            break;
        }
        case Animation::kAtom: {
            auto& p = animation_params_;
            p.electrons = std::clamp(p.electrons, 1, 20);
            static const int kShellCapacity[] = {2, 8, 8, 2};
            int shells[4] = {};
            int shell_count = 0;
            for (int left = p.electrons, s = 0; left > 0 && s < 4; s++) {
                shells[s] = std::min(left, kShellCapacity[s]);
                left -= shells[s];
                shell_count = s + 1;
            }
            lv_label_set_text(title_, kElements[p.electrons]);
            int len = snprintf(info, sizeof(info), "Shells:");
            for (int s = 0; s < shell_count; s++) {
                len += snprintf(info + len, sizeof(info) - len, "%s%d", s ? ", " : " ",
                                shells[s]);
            }
            info_label(LV_ALIGN_TOP_LEFT);
            int r_max = std::min(w, h) / 2 - 6;
            AddCircle(w / 2, h / 2, 14, Failure(), true);
            AddCenteredLabel(w / 2, h / 2, kElementSymbols[p.electrons], text_font_,
                             lv_color_white());
            for (int s = 0; s < shell_count; s++) {
                float r = shell_count == 1 ? 34.0f
                                           : 28 + (r_max - 28) * s / (shell_count - 1.0f);
                lv_obj_t* ring = AddCircle(w / 2, h / 2, static_cast<int>(r), Muted(), false, 1);
                lv_obj_set_style_border_opa(ring, LV_OPA_60, 0);
                float speed = (s % 2 ? -1.0f : 1.0f) * 2 * M_PI / (2.5f + s);
                for (int e = 0; e < shells[s]; e++) {
                    lv_obj_t* electron = AddCircle(0, 0, 4, Accent(), true);
                    movers_.push_back({electron, r, 2 * (float)M_PI * e / shells[s], speed, 8});
                }
            }
            break;
        }
    }
    TickAnimation(0);
    return "";
}

void StemBoard::TickAnimation(uint32_t elapsed) {
    const int w = canvas_w_;
    const int h = canvas_h_;
    float t = elapsed / 1000.0f;
    switch (animation_) {
        case Animation::kPendulum: {
            double length = animation_params_.length_cm / 100.0;
            double period = 2 * M_PI * sqrt(length / kGravity);
            double theta = animation_params_.angle * M_PI / 180 * cos(2 * M_PI * t / period);
            int rod = h - 30;
            int bx = w / 2 + static_cast<int>(rod * sin(theta));
            int by = 6 + static_cast<int>(rod * cos(theta));
            anim_points_[1] = {bx, by};
            lv_line_set_points_mutable(anim_line_, anim_points_, 2);
            lv_obj_set_pos(anim_body_, bx - 11, by - 11);
            break;
        }
        case Animation::kProjectile: {
            constexpr float kFlightSeconds = 2.5f;
            constexpr float kHoldSeconds = 1.5f;
            double theta = animation_params_.angle * M_PI / 180;
            double v = animation_params_.speed;
            double flight = 2 * v * sin(theta) / kGravity;
            double range = v * cos(theta) * flight;
            double height = pow(v * sin(theta), 2) / (2 * kGravity);
            // The flight stays below the two-line info label
            double scale =
                std::min((w - 24) / range, (h - 10 - 2 * text_font_->line_height) / height);
            float phase = fmodf(t, kFlightSeconds + kHoldSeconds);
            double sim_t = std::min(phase, kFlightSeconds) / kFlightSeconds * flight;
            auto position = [&](double tt, int32_t& x, int32_t& y) {
                x = 12 + static_cast<int32_t>(v * cos(theta) * tt * scale);
                y = h - 6 - static_cast<int32_t>((v * sin(theta) * tt - 0.5 * kGravity * tt * tt) *
                                                 scale);
            };
            int points =
                std::max(2, static_cast<int>(sim_t / flight * (anim_point_capacity_ - 1)) + 1);
            for (int i = 0; i < points; i++) {
                position(sim_t * i / (points - 1), anim_points_[i].x, anim_points_[i].y);
            }
            lv_line_set_points_mutable(anim_line_, anim_points_, points);
            int32_t x, y;
            position(sim_t, x, y);
            lv_obj_set_pos(anim_body_, x - 6, y - 6);
            break;
        }
        case Animation::kWave: {
            int mid = (h - title_h_) / 2;
            float amplitude = (h - title_h_) * 0.35f;
            float k = 2 * M_PI * animation_params_.count / w;
            float omega = M_PI;  // one full cycle every 2 seconds
            for (int i = 0; i < anim_point_capacity_; i++) {
                int x = (w - 1) * i / (anim_point_capacity_ - 1);
                int32_t y = mid - static_cast<int32_t>(amplitude * sinf(k * x - omega * t));
                anim_points_[i] = {x, y};
            }
            lv_line_set_points_mutable(anim_line_, anim_points_, anim_point_capacity_);
            int my = mid - static_cast<int>(amplitude * sinf(k * (w / 4) - omega * t));
            lv_obj_set_pos(anim_marker_, w / 4 - 6, my - 6);
            break;
        }
        case Animation::kOrbit:
        case Animation::kAtom:
            for (auto& m : movers_) {
                float a = m.phase + m.speed * t;
                lv_obj_set_pos(m.obj, w / 2 + static_cast<int>(m.radius * cosf(a)) - m.size / 2,
                               h / 2 - static_cast<int>(m.radius * sinf(a)) - m.size / 2);
            }
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// Formula cards

std::string StemBoard::ShowFormula(const std::string& name, const std::string& formula,
                                   const cJSON* symbols) {
    if (formula.empty()) {
        return "'formula' is required";
    }
    DisplayLockGuard lock(display_);
    Begin(name, Mode::kReveal);
    reveal_interval_ms_ = 700;
    reveal_highlight_ = false;

    lv_obj_t* list = lv_obj_create(canvas_);
    MakePlain(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(list, kPad, 0);

    lv_obj_t* f = AddLabel(list, Truncate(formula.c_str()).c_str(), large_font_,
                           lv_palette_main(LV_PALETTE_ORANGE));
    lv_obj_set_width(f, lv_pct(100));
    lv_label_set_long_mode(f, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(f, 4, 0);

    if (cJSON_IsArray(symbols)) {
        int count = 0;
        const cJSON* item;
        cJSON_ArrayForEach(item, symbols) {
            if (count++ >= kMaxSymbols) {
                break;
            }
            const char* symbol = StringItem(item, "symbol");
            const char* meaning = StringItem(item, "meaning");
            const char* unit = StringItem(item, "unit");
            if (symbol == nullptr || meaning == nullptr) {
                ClearLocked();
                return "Each symbol needs 'symbol' and 'meaning' strings (and optional 'unit')";
            }
            lv_obj_t* row = lv_obj_create(list);
            MakePlain(row);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_t* s = AddLabel(row, Truncate(symbol).c_str(), text_font_, Accent());
            lv_obj_set_width(s, text_font_->line_height * 2);
            lv_obj_t* m = AddLabel(row, Truncate(meaning).c_str(), text_font_, text_color_);
            lv_obj_set_flex_grow(m, 1);
            lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
            if (unit != nullptr && unit[0] != '\0') {
                std::string u = "(" + Truncate(unit) + ")";
                AddLabel(row, u.c_str(), text_font_, Muted());
            }
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            reveal_objs_.push_back(row);
        }
    }
    return "";
}
