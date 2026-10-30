#ifndef STEM_TOOLS_H
#define STEM_TOOLS_H

#include <string>

// Registers the self.stem.* MCP tools that let the model draw diagrams, graphs, step-by-step
// solutions, quizzes, simulations and formula cards on the screen. Does nothing when the
// display cannot show them (no LVGL theme, or a screen narrower than 200 px).
void RegisterStemTools();

// Forwards a TTS sentence that just started playing, so story mode can follow the speech.
// Call from the main task.
void StemOnSentence(const std::string& text);

#endif  // STEM_TOOLS_H
