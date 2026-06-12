#pragma once
#include <vector>
#include <cstdint>

// Low-level curve emitters. Each appends one Renderer::CURVE_FLOATS-sized
// curve (16 floats) to `out`. Bounding boxes are filled in for the tiling
// pass.

void emitFilledRect(std::vector<float>& out,
                    float cx, float cy, float halfW, float halfH,
                    float r, float g, float b, float a);

void emitLineSegment(std::vector<float>& out,
                     float x0, float y0, float x1, float y1,
                     float lineWidth,
                     float r, float g, float b, float a);

// Emits glyph strokes for one character at (x, y) with the given scale.
// The glyph is laid out in a unit box [0,1]x[0,1] where y=0 is the top
// (matching Vulkan pixel y-down). Strokes use the given lineWidth + colour.
//
// Recognised chars: 0-9, '.', '+', '-', 'x', '/', '=', '%', 'r' (radical),
// 'A', 'N', 'S', 'C'. Unknown chars are silently skipped.
void emitGlyph(std::vector<float>& out,
               char c, float x, float y, float scale, float lineWidth,
               float r, float g, float b, float a);

// Pixel advance (left edge to next char's left edge) for laying out strings.
float glyphAdvance(char c, float scale);
