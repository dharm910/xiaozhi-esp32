#include "stem_tools.h"

#include <esp_log.h>

#include <cstdlib>
#include <string>

#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "mcp_server.h"
#include "stem_board.h"

#define TAG "StemTools"

namespace {

constexpr size_t kMaxJsonArgument = 6000;
constexpr size_t kMaxStoryArgument = 10000;

// Set once at registration, from the main task; read from the main task.
StemBoard* stem_board = nullptr;

// Parses a JSON-valued string argument. Returns nullptr and sets `error` when it is invalid.
CJsonUniquePtr ParseJsonArgument(const PropertyList& properties, const char* name,
                                 std::string& error) {
    const auto& text = properties[name].value<std::string>();
    if (text.empty()) {
        return nullptr;
    }
    CJsonUniquePtr json(cJSON_Parse(text.c_str()));
    if (json == nullptr) {
        error = std::string("'") + name + "' is not valid JSON";
    }
    return json;
}

ToolResult Done(const std::string& error) {
    if (!error.empty()) {
        return std::unexpected(error);
    }
    return true;
}

double ParseDouble(const std::string& text, double fallback) {
    char* end = nullptr;
    double value = strtod(text.c_str(), &end);
    return end != text.c_str() ? value : fallback;
}

}  // namespace

void RegisterStemTools() {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr || display->GetTheme() == nullptr || display->width() < 200) {
        ESP_LOGI(TAG, "Display cannot show STEM content, tools not registered");
        return;
    }
    // Lives for the whole program, like the MCP server that holds the callbacks.
    auto board = new StemBoard(display);
    stem_board = board;
    auto& mcp = McpServer::GetInstance();
    std::string canvas = std::to_string(board->canvas_width()) + " x " +
                         std::to_string(board->canvas_height());

    mcp.AddTool(
        "self.stem.draw",
        "Draw a diagram on the device screen while you explain. Use it whenever a picture helps "
        "a student: geometry, force arrows, circuits, number lines, fractions, cells, etc. "
        "Shapes appear one by one in order, so list them in the order you talk about them.\n"
        "The canvas is " + canvas + " pixels, origin top-left, y grows downward.\n"
        "Args:\n"
        "  `title`: short heading shown above the drawing.\n"
        "  `shapes`: JSON array, at most 40. Each object has `type` and optional `color` "
        "(red, green, blue, orange, yellow, purple, cyan, pink, gray, brown, teal; default is "
        "the theme text color) and `width` (1-8):\n"
        "    {\"type\":\"line\",\"points\":[[x,y],...],\"closed\":false,\"dashed\":false,"
        "\"arrow\":false}\n"
        "    {\"type\":\"polygon\",\"points\":[[x,y],...]}  (outline only)\n"
        "    {\"type\":\"arrow\",\"from\":[x,y],\"to\":[x,y]}\n"
        "    {\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":10,\"fill\":false,\"radius\":0}\n"
        "    {\"type\":\"circle\",\"x\":0,\"y\":0,\"r\":10,\"fill\":false}\n"
        "    {\"type\":\"dot\",\"x\":0,\"y\":0}\n"
        "    {\"type\":\"arc\",\"x\":0,\"y\":0,\"r\":20,\"start\":0,\"end\":90}  (degrees, "
        "counter-clockwise from the +x axis; good for angle marks)\n"
        "    {\"type\":\"text\",\"x\":0,\"y\":0,\"text\":\"a = 3\",\"size\":\"small\"}  (centered "
        "on x,y; size small or large)\n"
        "  Text may use symbols like √ π θ Δ Ω ° ² ³ ≤ ≥ ≠ ≈ ∞ → × ÷ ± ∫ ∑ ✓.\n"
        "  `animate`: reveal shapes one at a time (default true).",
        PropertyList({Property("title", kPropertyTypeString, std::string("")).SetMaxLength(80),
                      Property("shapes", kPropertyTypeString).SetMaxLength(kMaxJsonArgument),
                      Property("animate", kPropertyTypeBoolean, true)}),
        [board](const PropertyList& properties) -> ToolResult {
            std::string error;
            auto shapes = ParseJsonArgument(properties, "shapes", error);
            if (!error.empty()) {
                return std::unexpected(error);
            }
            return Done(board->Draw(properties["title"].value<std::string>(), shapes.get(),
                                    properties["animate"].value<bool>()));
        });

    mcp.AddTool(
        "self.stem.plot",
        "Plot one to three functions of x on the device screen; the curves draw themselves "
        "from left to right. Use it for algebra, trigonometry, motion graphs, growth and decay.\n"
        "Args:\n"
        "  `expression`: e.g. \"x^2 - 4\" or \"sin(x); cos(x)\" (separate curves with ;). "
        "Supports + - * / ^, parentheses, implicit multiplication (2x), pi, e and sin cos tan "
        "asin acos atan sinh cosh tanh sqrt cbrt abs ln log log2 exp floor ceil sign.\n"
        "  `x_min`, `x_max`: x range as decimal strings (default \"-10\" and \"10\").\n"
        "  `y_range`: \"auto\" (default) or \"min,max\".\n"
        "  `marks`: optional JSON array of points to highlight, e.g. "
        "[{\"x\":2,\"y\":0,\"label\":\"root\"}].\n"
        "  `title`: optional heading; defaults to the expression.",
        PropertyList({Property("expression", kPropertyTypeString).SetMaxLength(200),
                      Property("x_min", kPropertyTypeString, std::string("-10")).SetMaxLength(20),
                      Property("x_max", kPropertyTypeString, std::string("10")).SetMaxLength(20),
                      Property("y_range", kPropertyTypeString, std::string("auto"))
                          .SetMaxLength(40),
                      Property("marks", kPropertyTypeString, std::string("")).SetMaxLength(1000),
                      Property("title", kPropertyTypeString, std::string("")).SetMaxLength(80)}),
        [board](const PropertyList& properties) -> ToolResult {
            std::string error;
            auto marks = ParseJsonArgument(properties, "marks", error);
            if (!error.empty()) {
                return std::unexpected(error);
            }
            return Done(board->Plot(properties["title"].value<std::string>(),
                                    properties["expression"].value<std::string>(),
                                    ParseDouble(properties["x_min"].value<std::string>(), -10),
                                    ParseDouble(properties["x_max"].value<std::string>(), 10),
                                    properties["y_range"].value<std::string>(), marks.get()));
        });

    mcp.AddTool(
        "self.stem.show_steps",
        "Show a worked solution on the device screen one step at a time, highlighting the step "
        "you are talking about. Use it whenever you solve a problem with more than one step.\n"
        "Also use it to check homework: after reading the student's work with "
        "self.camera.take_photo, list their lines, mark mistakes with status \"wrong\" and put "
        "the correction in `note`, and mark correct lines \"ok\".\n"
        "Args:\n"
        "  `title`: the problem, e.g. \"Solve 2x + 3 = 11\".\n"
        "  `steps`: JSON array (at most 12) of strings or objects "
        "{\"text\":\"2x = 8\",\"note\":\"subtract 3 from both sides\",\"status\":\"ok\"}; "
        "status is optional: ok or wrong.\n"
        "  `interval_ms`: delay between steps, roughly how long you speak per step "
        "(default 2500; 0 shows all at once).",
        PropertyList({Property("title", kPropertyTypeString, std::string("")).SetMaxLength(80),
                      Property("steps", kPropertyTypeString).SetMaxLength(kMaxJsonArgument),
                      Property("interval_ms", kPropertyTypeInteger, 2500, 0, 15000)}),
        [board](const PropertyList& properties) -> ToolResult {
            std::string error;
            auto steps = ParseJsonArgument(properties, "steps", error);
            if (!error.empty()) {
                return std::unexpected(error);
            }
            return Done(board->ShowSteps(properties["title"].value<std::string>(), steps.get(),
                                         properties["interval_ms"].value<int>()));
        });

    mcp.AddTool(
        "self.stem.quiz_ask",
        "Show a multiple-choice question on the device screen with options labelled A, B, C, D. "
        "Use it when the student asks to be quizzed or to check understanding after explaining "
        "something. Read the question and options aloud, wait for the student's answer, then "
        "call self.stem.quiz_answer.\n"
        "Args:\n"
        "  `question`: the question text.\n"
        "  `options`: JSON array of 2 to 4 option strings, e.g. [\"4\",\"6\",\"8\"].",
        PropertyList({Property("question", kPropertyTypeString).SetMaxLength(200),
                      Property("options", kPropertyTypeString).SetMaxLength(1000)}),
        [board](const PropertyList& properties) -> ToolResult {
            std::string error;
            auto options = ParseJsonArgument(properties, "options", error);
            if (!error.empty()) {
                return std::unexpected(error);
            }
            return Done(board->AskQuiz(properties["question"].value<std::string>(), options.get()));
        });

    mcp.AddTool(
        "self.stem.quiz_answer",
        "Reveal the answer to the question shown by self.stem.quiz_ask: highlights the correct "
        "option, celebrates a right answer and updates the score and streak on screen. Returns "
        "the outcome and running score.\n"
        "Args:\n"
        "  `correct_option`: letter of the correct option, e.g. \"B\".\n"
        "  `student_answer`: letter the student chose (map a spoken answer to its letter), or "
        "\"\" if they did not answer.",
        PropertyList({Property("correct_option", kPropertyTypeString).SetMaxLength(1),
                      Property("student_answer", kPropertyTypeString, std::string(""))
                          .SetMaxLength(1)}),
        [board](const PropertyList& properties) -> ToolResult {
            std::string result;
            auto error = board->AnswerQuiz(properties["correct_option"].value<std::string>(),
                                           properties["student_answer"].value<std::string>(),
                                           result);
            if (!error.empty()) {
                return std::unexpected(error);
            }
            return result;
        });

    mcp.AddTool(
        "self.stem.animate",
        "Run a live physics or science simulation on the device screen while you explain it. "
        "The simulation uses real formulas and shows the computed values.\n"
        "Args:\n"
        "  `name`: one of\n"
        "    pendulum   - uses `length_cm` (10-1000) and `angle` (amplitude, 1-60 degrees)\n"
        "    projectile - uses `speed` (1-100 m/s) and `angle` (launch angle, 5-85 degrees)\n"
        "    wave       - travelling sine wave; `count` = cycles shown (1-5)\n"
        "    orbit      - planets obeying Kepler's third law; `count` = planets (1-6)\n"
        "    atom       - Bohr model; `electrons` = atomic number (1-20)\n"
        "  Unused arguments are ignored; `angle` 0 means the default for that simulation.",
        PropertyList({Property("name", kPropertyTypeString).SetMaxLength(20),
                      Property("angle", kPropertyTypeInteger, 0, 0, 90),
                      Property("speed", kPropertyTypeInteger, 20, 1, 100),
                      Property("length_cm", kPropertyTypeInteger, 100, 10, 1000),
                      Property("electrons", kPropertyTypeInteger, 6, 1, 20),
                      Property("count", kPropertyTypeInteger, 4, 1, 6)}),
        [board](const PropertyList& properties) -> ToolResult {
            StemBoard::AnimationParams params;
            params.name = properties["name"].value<std::string>();
            params.angle = properties["angle"].value<int>();
            params.speed = properties["speed"].value<int>();
            params.length_cm = properties["length_cm"].value<int>();
            params.electrons = properties["electrons"].value<int>();
            params.count = properties["count"].value<int>();
            return Done(board->Animate(params));
        });

    mcp.AddTool(
        "self.stem.formula",
        "Show a formula card on the device screen: the formula in large type and what each "
        "symbol means with its unit. Use it whenever you introduce or use a formula.\n"
        "Args:\n"
        "  `name`: e.g. \"Newton's second law\".\n"
        "  `formula`: e.g. \"F = m × a\" (symbols like √ π Δ ² ° are supported).\n"
        "  `symbols`: JSON array (at most 8) of {\"symbol\":\"F\",\"meaning\":\"force\","
        "\"unit\":\"N\"}.",
        PropertyList({Property("name", kPropertyTypeString, std::string("")).SetMaxLength(80),
                      Property("formula", kPropertyTypeString).SetMaxLength(120),
                      Property("symbols", kPropertyTypeString, std::string(""))
                          .SetMaxLength(2000)}),
        [board](const PropertyList& properties) -> ToolResult {
            std::string error;
            auto symbols = ParseJsonArgument(properties, "symbols", error);
            if (!error.empty()) {
                return std::unexpected(error);
            }
            return Done(board->ShowFormula(properties["name"].value<std::string>(),
                                           properties["formula"].value<std::string>(),
                                           symbols.get()));
        });

    mcp.AddTool(
        "self.stem.story",
        "Tell an animated story on the device screen: a short film of scenes, each with a "
        "caption you speak while pictures appear, move, grow and change. Prefer it for "
        "explaining processes and cause-and-effect (water cycle, photosynthesis, circuits, "
        "digestion, food chains, moon phases, chemical reactions, how machines work).\n"
        "IMPORTANT: after calling this tool, speak each scene's `say` text, in order, word for "
        "word, one scene per sentence; scenes change exactly when you start saying them.\n"
        "The canvas is " + canvas + " pixels, origin top-left; the caption covers the bottom "
        "~60 px, so keep key objects above that.\n"
        "Args:\n"
        "  `title`: short heading.\n"
        "  `scenes`: JSON array (at most 12) of {\"say\":\"...\",\"actions\":[...]}. Objects "
        "persist across scenes until removed. Each action has `do`, `id` (object name), "
        "optional `at` (ms after the scene starts) and `ms` (duration, default 800):\n"
        "    add:    {\"do\":\"add\",\"id\":\"sun\",\"icon\":\"sun\",\"x\":40,\"y\":30,"
        "\"size\":1.5,\"enter\":\"slide_top\"}  icon centered on x,y (44 px at size 1).\n"
        "            Or a text {\"type\":\"text\",\"text\":\"CO₂\",\"x\":..,\"y\":..,"
        "\"size\":\"large\"} or any shape {\"type\":\"line|polygon|arrow|rect|circle|dot|arc\", "
        "...same fields as self.stem.draw}.\n"
        "            `enter`: fade (default), grow, slide_left, slide_right, slide_top, "
        "slide_bottom, none. Optional `color`.\n"
        "    move:   {\"do\":\"move\",\"id\":\"drop\",\"to\":[x,y],\"ms\":1000,\"repeat\":false}\n"
        "    path:   {\"do\":\"path\",\"id\":\"electron\",\"points\":[[x,y],...],\"ms\":2000,"
        "\"repeat\":true}  follow a route; repeat loops it (flows, circuits, orbits).\n"
        "    scale:  {\"do\":\"scale\",\"id\":\"plant\",\"to\":2}  icons and text only.\n"
        "    fade:   {\"do\":\"fade\",\"id\":\"x\",\"to\":0.3}   color: "
        "{\"do\":\"color\",\"id\":\"x\",\"to\":\"green\"}\n"
        "    pulse:  {\"do\":\"pulse\",\"id\":\"x\",\"times\":3}  draw attention.\n"
        "    text:   {\"do\":\"text\",\"id\":\"label\",\"text\":\"new text\"}\n"
        "    remove: {\"do\":\"remove\",\"id\":\"x\"}\n"
        "  Icons: " + StemBoard::IconNames() + ".\n"
        "  `replay`: true replays the last story (then `scenes` can be empty), e.g. when the "
        "student says \"show me again\".",
        PropertyList({Property("title", kPropertyTypeString, std::string("")).SetMaxLength(80),
                      Property("scenes", kPropertyTypeString, std::string(""))
                          .SetMaxLength(kMaxStoryArgument),
                      Property("replay", kPropertyTypeBoolean, false)}),
        [board](const PropertyList& properties) -> ToolResult {
            if (properties["replay"].value<bool>()) {
                return Done(board->ReplayStory());
            }
            std::string error;
            auto scenes = ParseJsonArgument(properties, "scenes", error);
            if (!error.empty()) {
                return std::unexpected(error);
            }
            if (scenes == nullptr) {
                return std::unexpected("'scenes' is required unless replay is true");
            }
            return Done(board->Story(properties["title"].value<std::string>(), scenes.get()));
        });

    mcp.AddTool("self.stem.clear",
                "Remove the diagram, graph, steps, quiz, simulation, formula card or story from "
                "the device screen. Content also disappears by itself after two minutes.",
                PropertyList(), [board](const PropertyList& properties) -> ToolResult {
                    board->Clear();
                    return true;
                });
}

void StemOnSentence(const std::string& text) {
    if (stem_board != nullptr) {
        stem_board->OnSentence(text);
    }
}
