#ifndef STEM_BOARD_H
#define STEM_BOARD_H

#include <cJSON.h>
#include <lvgl.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cjson_utils.h"

class Display;
class LvglTheme;

// A full-width overlay below the status bar that shows STEM teaching content: diagrams,
// function graphs, step-by-step solutions, quiz cards, small physics simulations, formula
// cards, and story mode: animated scenes timed to the spoken sentences. Public methods are
// called from the main task and take the display lock; the overlay's own timer and
// animations run in the LVGL task, which already holds it.
// Every method returns an empty string on success or an error message for the model.
class StemBoard {
public:
    struct AnimationParams {
        std::string name;
        int angle = 0;       // degrees; 0 means the animation's default
        int speed = 20;      // projectile launch speed, m/s
        int length_cm = 100; // pendulum length
        int electrons = 6;   // atom
        int count = 4;       // orbit: number of planets; wave: number of cycles shown
    };

    explicit StemBoard(Display* display);
    ~StemBoard();

    // Drawing area available to diagrams, in pixels, origin at its top-left corner.
    int canvas_width() const { return canvas_w_; }
    int canvas_height() const { return canvas_h_; }

    std::string Draw(const std::string& title, const cJSON* shapes, bool animate);
    std::string Plot(const std::string& title, const std::string& expressions, double x_min,
                     double x_max, const std::string& y_range, const cJSON* marks);
    std::string ShowSteps(const std::string& title, const cJSON* steps, int interval_ms);
    std::string AskQuiz(const std::string& question, const cJSON* options);
    // On success `result` describes the outcome and the running score for the model.
    std::string AnswerQuiz(const std::string& correct_option, const std::string& student_answer,
                           std::string& result);
    std::string Animate(const AnimationParams& params);
    std::string ShowFormula(const std::string& name, const std::string& formula,
                            const cJSON* symbols);
    // Story mode. `scenes` is copied, so the story can be replayed later.
    std::string Story(const std::string& title, const cJSON* scenes);
    std::string ReplayStory();
    // Called when the server starts speaking a sentence; moves the story to the matching scene.
    void OnSentence(const std::string& text);
    // Called when a story scene starts, and when content closes by itself. Either may run in
    // the LVGL task or the main task, with the display lock held.
    void SetListeners(std::function<void(int scene)> on_scene, std::function<void()> on_closed) {
        on_scene_ = std::move(on_scene);
        on_closed_ = std::move(on_closed);
    }
    // Comma-separated names of the pictures story mode can show.
    static std::string IconNames();
    void Clear();

private:
    enum class Mode { kNone, kReveal, kPlot, kAnimation, kQuiz, kStory };
    enum class Animation { kPendulum, kProjectile, kWave, kOrbit, kAtom };

    struct PlotSegment {
        lv_obj_t* line;
        lv_point_precise_t* points;
        int count;
        int first_sample;  // index of the first point in the whole plot's sample order
    };
    struct Particle {
        lv_obj_t* obj;
        float vx, vy;
    };
    struct StoryObject {
        std::string id;
        lv_obj_t* obj;
        int off_x, off_y;  // object position = anchor point - offset
        bool scalable;     // icons and text; shapes live on full-canvas layers and cannot scale
        bool is_text;
        int32_t scale;     // intended transform scale (256 = 1x), even while an animation runs
    };
    struct PendingAction {
        uint32_t at;
        const cJSON* action;
    };
    struct PathData {
        std::vector<lv_point_t> points;
        std::vector<float> lengths;  // cumulative length at each point
        int off_x, off_y;
    };
    struct Mover {
        lv_obj_t* obj;
        float radius;  // orbit radius in pixels
        float phase;
        float speed;   // radians per second
        int size;
    };

    Display* display_;
    int top_offset_ = 0;
    int title_h_ = 0;
    int canvas_w_ = 0;
    int canvas_h_ = 0;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint32_t shown_tick_ = 0;
    uint32_t mode_tick_ = 0;
    uint32_t close_after_ms_ = 0;
    Mode mode_ = Mode::kNone;
    std::function<void(int)> on_scene_;
    std::function<void()> on_closed_;

    const lv_font_t* text_font_ = nullptr;
    const lv_font_t* large_font_ = nullptr;
    lv_color_t text_color_;
    lv_color_t background_color_;

    std::vector<std::unique_ptr<lv_point_precise_t[]>> point_buffers_;

    // Reveal: objects appear one at a time; with highlight, the newest one is highlighted
    std::vector<lv_obj_t*> reveal_objs_;
    uint32_t reveal_interval_ms_ = 0;
    size_t revealed_ = 0;
    bool reveal_highlight_ = false;

    // Plot: curves draw themselves from left to right
    std::vector<PlotSegment> plot_segments_;
    int plot_samples_ = 0;

    // Animation
    Animation animation_ = Animation::kPendulum;
    AnimationParams animation_params_;
    lv_obj_t* anim_line_ = nullptr;
    lv_point_precise_t* anim_points_ = nullptr;
    int anim_point_capacity_ = 0;
    lv_obj_t* anim_body_ = nullptr;
    lv_obj_t* anim_marker_ = nullptr;
    std::vector<Mover> movers_;

    // Quiz: score survives between questions until reboot
    std::vector<lv_obj_t*> quiz_options_;
    bool quiz_answered_ = false;
    int quiz_correct_ = 0;
    int quiz_total_ = 0;
    int quiz_streak_ = 0;
    std::vector<Particle> particles_;
    uint32_t particles_tick_ = 0;

    // Story: story_ outlives ClearLocked() so it can be replayed
    CJsonUniquePtr story_;
    std::string story_title_;
    std::vector<const cJSON*> scenes_;
    int scene_ = -1;
    uint32_t scene_tick_ = 0;
    uint32_t scene_duration_ = 0;
    bool story_synced_ = false;
    std::vector<PendingAction> pending_;
    std::vector<StoryObject> story_objects_;
    std::vector<std::unique_ptr<PathData>> paths_;
    lv_obj_t* caption_ = nullptr;
    const lv_font_t* icon_font_ = nullptr;

    static void OnTimer(lv_timer_t* timer);
    void Tick();
    void TickReveal(uint32_t elapsed);
    void TickPlot(uint32_t elapsed);
    void TickAnimation(uint32_t elapsed);
    void TickParticles();
    void TickStory();
    static void ExecPathAnim(lv_anim_t* anim, int32_t v);

    std::string ValidateStory(const cJSON* scenes) const;
    void StartStory();
    void StartScene(int index);
    void JumpToScene(int index);
    void ExecuteAction(const cJSON* action, bool instant);
    StoryObject* FindStoryObject(const char* id);
    bool BuildIcon(const char* name, lv_obj_t* box);
    void FillTriangle(int tip_x, int tip_y, int x0, int x1, int base_y, lv_color_t color);
    lv_obj_t* AddRect(int x, int y, int w, int h, lv_color_t color, bool fill, int radius = 0);

    void Begin(const std::string& title, Mode mode);
    void ClearLocked();
    void UpdateQuizTitle();
    void StartBurst(lv_color_t color);
    LvglTheme* theme() const;

    lv_point_precise_t* AllocPoints(int count);
    lv_obj_t* AddLine(lv_point_precise_t* points, int count, lv_color_t color, int width,
                      bool dashed = false);
    lv_obj_t* AddCircle(int cx, int cy, int r, lv_color_t color, bool fill, int border = 2);
    lv_obj_t* AddLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                       lv_color_t color);
    lv_obj_t* AddCenteredLabel(int cx, int cy, const char* text, const lv_font_t* font,
                               lv_color_t color);
    lv_obj_t* AddArrowHead(int x0, int y0, int x1, int y1, lv_color_t color, int width);
    std::string AddShape(const cJSON* shape, lv_obj_t* layer);
    lv_color_t ParseColor(const cJSON* item, lv_color_t fallback) const;
};

#endif  // STEM_BOARD_H
