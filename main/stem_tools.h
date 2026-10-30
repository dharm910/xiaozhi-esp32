#ifndef STEM_TOOLS_H
#define STEM_TOOLS_H

// Registers the self.stem.* MCP tools that let the model draw diagrams, graphs, step-by-step
// solutions, quizzes, simulations and formula cards on the screen. Does nothing when the
// display cannot show them (no LVGL theme, or a screen narrower than 200 px).
void RegisterStemTools();

#endif  // STEM_TOOLS_H
