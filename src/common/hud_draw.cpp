// The modern HUD's drawing: the same two corner groups the game draws, as vectors.
//
// The game's HUD is authored for 640x480 and the host stretches that over the output, so at 4K its
// bars and glyphs are soft and its text is unreadable at a glance. Everything here is built from
// ImGui draw-list primitives at the output's own resolution and scaled from it, so it is sharp at
// any size; nothing is sampled from a texture.
//
// Layout follows the original so the player's eye does not have to move: the reputation number,
// the bar toward the next unlock and the clock along the top left; the mood dial and the five
// motive bars along the bottom left, in the game's order - bladder, hygiene, hunger, fun, energy.
//
// This file is only the drawing. The values come from hud::Read (hud_state.cpp), the setting from
// hud::g_enabled, and the call from overlay.cpp inside the ImGui frame.
#include "common/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <imgui.h>

namespace hud {

bool g_enabled = true;

namespace {

// the shape of the whole HUD at 1080p; every length below is multiplied by the output's scale
constexpr float kBase = 1080.0f;
constexpr float kMargin = 28.0f;
constexpr float kBarW = 132.0f, kBarH = 12.0f, kBarGap = 16.0f, kIcon = 26.0f;
constexpr float kDial = 58.0f;  // mood dial radius

// The game draws 640x448 inside its 640x480 frame and the host now presents those rows alone
// (app::GameInfo::picture_top/height), so the window is all picture and the HUD starts at its very
// top. The game's own HUD ended at 0.894 of the uncropped output height; over the cropped picture
// that row is (0.894 * 480 - 16) / 448 = 0.922, and the modern HUD sits on that same floor so the
// eye does not have to move between the two styles.
constexpr float kBandTop = 0.0f;
constexpr float kLegacyFloor = 0.922f;

ImU32 Rgba(float r, float g, float b, float a) { return ImGui::GetColorU32(ImVec4(r, g, b, a)); }

// a motive's colour: red when it is about to hurt, amber in the middle, green when it is fine
ImU32 Level(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    if (v < 0.5f) {
        float t = v / 0.5f;  // red to amber
        return Rgba(0.90f, 0.25f + 0.45f * t, 0.20f, 0.95f);
    }
    float t = (v - 0.5f) / 0.5f;  // amber to green
    return Rgba(0.90f - 0.55f * t, 0.70f + 0.10f * t, 0.20f + 0.15f * t, 0.95f);
}

// text with a soft shadow under it: legible over both a bright pavement and a night street
void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 col, const char* text) {
    dl->AddText(font, size, ImVec2(at.x + size * 0.06f, at.y + size * 0.06f), Rgba(0, 0, 0, 0.55f), text);
    dl->AddText(font, size, at, col, text);
}

ImVec2 TextSize(ImFont* font, float size, const char* text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

// a rounded capsule with a dark bed and a filled portion, the shape every bar here uses
void Bar(ImDrawList* dl, ImVec2 at, float w, float h, float v, ImU32 fill) {
    const float r = h * 0.5f;
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), Rgba(0, 0, 0, 0.55f), r);
    const float fw = std::clamp(v, 0.0f, 1.0f) * w;
    if (fw > 0.5f) dl->AddRectFilled(at, ImVec2(at.x + std::max(fw, h), at.y + h), fill, r);
    dl->AddRect(at, ImVec2(at.x + w, at.y + h), Rgba(1, 1, 1, 0.18f), r, 0, std::max(1.0f, h * 0.08f));
}

// A five-pointed star, used for the reputation marker. Points alternate between the outer and the
// inner radius, starting at the top.
void Star(ImDrawList* dl, ImVec2 c, float outer, ImU32 col) {
    ImVec2 p[10];
    for (int i = 0; i < 10; i++) {
        const float a = -1.5707964f + float(i) * 0.62831855f;  // -90 degrees, then 36 degrees a step
        const float r = (i & 1) ? outer * 0.42f : outer;
        p[i] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
    }
    dl->AddConvexPolyFilled(p, 10, col);
}

// --- the motive glyphs, drawn as line art so they stay sharp at any size ---

// bladder: a toilet seen from the side, the way the game's own icon draws it. The first attempt
// put a narrow arc on a thin stem and read as a wine glass, so the bowl is now a wide tapered
// basin with a flat seat across the top, a short thick pedestal and a floor line - the silhouette
// that says toilet at 26 pixels.
void IconToilet(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float t) {
    const float cx = c.x - s * 0.04f;
    // cistern at the back, tall and narrow
    dl->AddRect(ImVec2(cx + s * 0.20f, c.y - s * 0.54f), ImVec2(cx + s * 0.44f, c.y - s * 0.10f), col,
                s * 0.05f, 0, t);
    // seat: a flat lid across the bowl, from the front lip back to the cistern
    dl->AddLine(ImVec2(cx - s * 0.40f, c.y - s * 0.14f), ImVec2(cx + s * 0.20f, c.y - s * 0.14f), col, t * 1.1f);
    // bowl: wide at the seat, tapering to the pedestal
    dl->AddLine(ImVec2(cx - s * 0.38f, c.y - s * 0.12f), ImVec2(cx - s * 0.16f, c.y + s * 0.22f), col, t);
    dl->AddLine(ImVec2(cx + s * 0.18f, c.y - s * 0.12f), ImVec2(cx + s * 0.06f, c.y + s * 0.22f), col, t);
    dl->AddLine(ImVec2(cx - s * 0.16f, c.y + s * 0.22f), ImVec2(cx + s * 0.06f, c.y + s * 0.22f), col, t);
    // pedestal down to the floor
    dl->AddLine(ImVec2(cx - s * 0.12f, c.y + s * 0.22f), ImVec2(cx - s * 0.12f, c.y + s * 0.44f), col, t);
    dl->AddLine(ImVec2(cx + s * 0.02f, c.y + s * 0.22f), ImVec2(cx + s * 0.02f, c.y + s * 0.44f), col, t);
    dl->AddLine(ImVec2(cx - s * 0.26f, c.y + s * 0.46f), ImVec2(cx + s * 0.16f, c.y + s * 0.46f), col, t * 1.1f);
}

void IconShower(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float t) {
    // head and its arm
    dl->AddLine(ImVec2(c.x + s * 0.34f, c.y - s * 0.52f), ImVec2(c.x + s * 0.34f, c.y - s * 0.24f), col, t);
    dl->AddLine(ImVec2(c.x - s * 0.40f, c.y - s * 0.24f), ImVec2(c.x + s * 0.40f, c.y - s * 0.24f), col, t);
    for (int i = 0; i < 3; i++) {
        const float x = c.x - s * 0.26f + float(i) * s * 0.26f;
        dl->AddLine(ImVec2(x, c.y - s * 0.06f), ImVec2(x - s * 0.08f, c.y + s * 0.5f), col, t);
    }
}

void IconFork(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float t) {
    const float top = c.y - s * 0.52f, bot = c.y + s * 0.52f;
    // fork: three tines joining a stem
    for (int i = -1; i <= 1; i++) {
        const float x = c.x - s * 0.30f + float(i) * s * 0.13f;
        dl->AddLine(ImVec2(x, top), ImVec2(x, c.y - s * 0.12f), col, t);
    }
    dl->AddLine(ImVec2(c.x - s * 0.43f, c.y - s * 0.12f), ImVec2(c.x - s * 0.17f, c.y - s * 0.12f), col, t);
    dl->AddLine(ImVec2(c.x - s * 0.30f, c.y - s * 0.12f), ImVec2(c.x - s * 0.30f, bot), col, t);
    // knife
    dl->AddLine(ImVec2(c.x + s * 0.26f, top), ImVec2(c.x + s * 0.26f, bot), col, t);
    dl->AddBezierQuadratic(ImVec2(c.x + s * 0.26f, top), ImVec2(c.x + s * 0.44f, c.y - s * 0.2f),
                           ImVec2(c.x + s * 0.26f, c.y + s * 0.06f), col, t);
}

void IconNote(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float t) {
    const float stem = c.x + s * 0.22f;
    dl->AddLine(ImVec2(stem, c.y + s * 0.30f), ImVec2(stem, c.y - s * 0.50f), col, t);
    dl->AddBezierQuadratic(ImVec2(stem, c.y - s * 0.50f), ImVec2(stem + s * 0.40f, c.y - s * 0.38f),
                           ImVec2(stem + s * 0.12f, c.y - s * 0.14f), col, t);
    dl->AddCircle(ImVec2(stem - s * 0.20f, c.y + s * 0.30f), s * 0.21f, col, 0, t);
}

void IconBed(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float t) {
    const float l = c.x - s * 0.46f, r = c.x + s * 0.46f, top = c.y - s * 0.18f, bot = c.y + s * 0.36f;
    dl->AddLine(ImVec2(l, c.y - s * 0.44f), ImVec2(l, bot), col, t);        // headboard post
    dl->AddLine(ImVec2(l, bot), ImVec2(r, bot), col, t);                    // base
    dl->AddLine(ImVec2(r, c.y + s * 0.06f), ImVec2(r, bot), col, t);        // foot
    dl->AddRect(ImVec2(l, top), ImVec2(r, c.y + s * 0.06f), col, s * 0.08f, 0, t);  // mattress
    dl->AddLine(ImVec2(l + s * 0.06f, top), ImVec2(l + s * 0.26f, top), col, t);    // pillow
}

using IconFn = void (*)(ImDrawList*, ImVec2, float, ImU32, float);

// the mood dial: a ring with an arc filled to the mood, and a face that follows it
void MoodDial(ImDrawList* dl, ImVec2 c, float r, float mood, float t) {
    const ImU32 col = Level(mood);
    dl->AddCircleFilled(c, r, Rgba(0, 0, 0, 0.5f), 48);
    dl->AddCircle(c, r, Rgba(1, 1, 1, 0.22f), 48, t);
    // the arc runs clockwise from the bottom, so a full mood closes the ring
    const float a0 = 1.5707964f, a1 = a0 + 6.2831853f * std::clamp(mood, 0.0f, 1.0f);
    dl->PathArcTo(c, r, a0, a1, 64);
    dl->PathStroke(col, 0, t * 2.2f);
    // eyes and a mouth that curves with the mood: a smile at 1, a frown at 0
    const float eye = r * 0.13f;
    dl->AddCircleFilled(ImVec2(c.x - r * 0.32f, c.y - r * 0.22f), eye, col, 12);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.32f, c.y - r * 0.22f), eye, col, 12);
    const float curve = (std::clamp(mood, 0.0f, 1.0f) - 0.5f) * r * 0.9f;
    dl->AddBezierQuadratic(ImVec2(c.x - r * 0.38f, c.y + r * 0.22f), ImVec2(c.x, c.y + r * 0.22f + curve),
                           ImVec2(c.x + r * 0.38f, c.y + r * 0.22f), col, t * 1.6f);
}

}  // namespace

void Draw(const State& s) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) return;
    // the HUD belongs under any panel the overlay opens, and must never take a click
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float k = std::max(io.DisplaySize.y / kBase, 0.5f);  // the output's scale
    const float t = std::max(1.0f, 1.6f * k);                  // line weight
    const ImU32 white = Rgba(1, 1, 1, 0.95f);
    const float top = kBandTop * io.DisplaySize.y;         // first row of picture
    const float floor_y = kLegacyFloor * io.DisplaySize.y;  // where the game's own HUD ended

    // --- top left: the reputation number, the bar to the next unlock, the clock ---
    {
        const float x = kMargin * k, y = top + kMargin * k;
        const float num = 30.0f * k, boxh = 38.0f * k;
        char rep[16];
        std::snprintf(rep, sizeof rep, "%d", s.rep);
        const ImVec2 ls = TextSize(font, num, rep);
        float boxw = std::max(boxh, ls.x + 22.0f * k);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + boxw, y + boxh), Rgba(0, 0, 0, 0.55f), boxh * 0.3f);
        dl->AddRect(ImVec2(x, y), ImVec2(x + boxw, y + boxh), Rgba(1.0f, 0.78f, 0.25f, 0.9f), boxh * 0.3f, 0, t);
        Text(dl, font, num, ImVec2(x + (boxw - ls.x) * 0.5f, y + (boxh - ls.y) * 0.5f), white, rep);
        boxw += 14.0f * k;

        const float bx = x + boxw, by = y + boxh * 0.5f - 5.0f * k;
        const float bw = 150.0f * k, bh = 10.0f * k;
        Bar(dl, ImVec2(bx, by), bw, bh, s.progress, Rgba(1.0f, 0.78f, 0.25f, 0.95f));
        Star(dl, ImVec2(bx + bw + 16.0f * k, y + boxh * 0.5f), 14.0f * k, Rgba(1.0f, 0.78f, 0.25f, 0.95f));

        const int h12 = s.hour % 12 == 0 ? 12 : s.hour % 12;
        char clock[16];
        std::snprintf(clock, sizeof clock, "%d:%02d %s", h12, s.minute, s.hour < 12 ? "am" : "pm");
        Text(dl, font, 28.0f * k, ImVec2(bx + bw + 38.0f * k, y + boxh * 0.5f - 14.0f * k), white, clock);
    }

    // --- bottom left: the mood dial, then the five motives ---
    {
        const float r = kDial * 0.5f * k;
        const float base = floor_y;
        const ImVec2 c(kMargin * k + r, base - r);
        MoodDial(dl, c, r, s.mood, t);

        const float x0 = c.x + r + 26.0f * k;
        const float bw = kBarW * k, bh = kBarH * k, gap = kBarGap * k, icon = kIcon * k;
        const float by = base - bh;                   // the bars sit on the bottom margin
        const float iy = by - icon * 0.5f - 10.0f * k;  // glyphs above their bar
        const float values[5] = {s.bladder, s.hygiene, s.hunger, s.fun, s.energy};
        const IconFn icons[5] = {IconToilet, IconShower, IconFork, IconNote, IconBed};
        for (int i = 0; i < 5; i++) {
            const float x = x0 + float(i) * (bw + gap);
            const ImU32 col = Level(values[i]);
            icons[i](dl, ImVec2(x + bw * 0.5f, iy), icon, col, t);
            Bar(dl, ImVec2(x, by), bw, bh, values[i], col);
        }
    }

    // --- bottom right: the action queue ---
    //
    // The original is a translucent purple strip of rendered thumbnails, one per queued action.
    // Those pictures are drawn by the game into its own clips and cannot be reproduced as
    // vectors, so the strip is redrawn from the queue's own state instead: one chip per queued
    // action, oldest on the left, the running one filled and marked with a play triangle. It
    // sits on the same floor as the motives and is only there when something is queued.
    if (s.queued > 0) {
        const float chip = 30.0f * k, gap = 10.0f * k;
        const float width = float(s.queued) * chip + float(s.queued - 1) * gap;
        const float right = io.DisplaySize.x - kMargin * k;
        const float y = floor_y - chip;
        for (int i = 0; i < s.queued; i++) {
            const float x = right - width + float(i) * (chip + gap);
            const bool running = i == s.queue_active;
            const ImVec2 a(x, y), b(x + chip, y + chip);
            dl->AddRectFilled(a, b, Rgba(0, 0, 0, running ? 0.62f : 0.45f), chip * 0.28f);
            dl->AddRect(a, b, running ? Rgba(1.0f, 0.78f, 0.25f, 0.95f) : Rgba(1, 1, 1, 0.22f),
                        chip * 0.28f, 0, running ? t * 1.4f : t);
            const ImVec2 c(x + chip * 0.5f, y + chip * 0.5f);
            if (running) {
                // a play triangle: this is the one the sim is doing
                const float r = chip * 0.22f;
                const ImVec2 p[3] = {ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x - r * 0.7f, c.y + r),
                                     ImVec2(c.x + r, c.y)};
                dl->AddConvexPolyFilled(p, 3, Rgba(1.0f, 0.78f, 0.25f, 0.95f));
            } else {
                // queued: a small ring, the same shape the running chip fills in
                dl->AddCircle(c, chip * 0.18f, Rgba(1, 1, 1, 0.5f), 20, t);
            }
        }
    }
}


// The NPC dialogue panel. The game's own box is a stretched 640x480 bitmap with a hard black
// border and pixelated text; this is a soft card with a shoulder of accent colour on the speaker
// side, the speaker's name above the body, and the game's own coloured words kept as spans so a
// name or an emphasised word still reads the way the writer meant.
//
// It sits where the original sits - the upper left band of the screen - and scales from the
// output height like the rest of the HUD, so the text is the same physical size at 1080p and 4K.
void DrawChat(const Chat& c) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f || c.body[0] == '\0') return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float k = std::max(io.DisplaySize.y / kBase, 0.5f);
    const float pad = 22.0f * k, name_size = 24.0f * k, body_size = 27.0f * k;
    const float x = kMargin * k, y = 96.0f * k;  // below the reputation row, where the game's box is
    const float w = std::min(io.DisplaySize.x * 0.62f, 980.0f * k);

    // lay the body out by words so it wraps inside the card, keeping each word's span colour
    struct Line {
        int first = 0, last = 0;
    };
    Line lines[12];
    int line_count = 1;
    const float inner = w - pad * 2.0f;
    float run = 0.0f;
    int word_start = 0;
    const int len = int(std::strlen(c.body));
    for (int i = 0; i <= len; i++) {
        const bool hard = c.body[i] == '\n';
        if (c.body[i] != ' ' && c.body[i] != '\0' && !hard) continue;
        const float wl = font->CalcTextSizeA(body_size, FLT_MAX, 0.0f, c.body + word_start, c.body + i).x;
        const float space = run > 0.0f ? font->CalcTextSizeA(body_size, FLT_MAX, 0.0f, " ").x : 0.0f;
        if (run + space + wl > inner && line_count < int(std::size(lines))) {
            lines[line_count - 1].last = word_start - 1;
            lines[line_count].first = word_start;
            line_count++;
            run = wl;
        } else {
            run += space + wl;
        }
        lines[line_count - 1].last = i;
        if (hard && line_count < int(std::size(lines))) {  // the game's own line break
            lines[line_count].first = i + 1;
            line_count++;
            run = 0.0f;
        }
        word_start = i + 1;
    }

    const float body_h = float(line_count) * body_size * 1.22f;
    const float h = pad * 2.0f + name_size * 1.5f + body_h;
    // the card: a dark translucent slab, a soft edge, and an accent rail down the speaker's side
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Rgba(0.04f, 0.05f, 0.07f, 0.82f), 14.0f * k);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), Rgba(1, 1, 1, 0.14f), 14.0f * k, 0, std::max(1.0f, 1.4f * k));
    dl->AddRectFilled(ImVec2(x, y + 10.0f * k), ImVec2(x + 5.0f * k, y + h - 10.0f * k),
                      Rgba(1.0f, 0.62f, 0.22f, 0.95f), 3.0f * k);

    if (c.speaker[0])
        Text(dl, font, name_size, ImVec2(x + pad, y + pad * 0.6f), Rgba(1.0f, 0.62f, 0.22f, 1.0f), c.speaker);

    // each line is drawn in runs, so a span's colour applies to exactly the characters it covers
    float ty = y + pad * 0.6f + name_size * 1.5f;
    for (int l = 0; l < line_count; l++) {
        float tx = x + pad;
        int i = lines[l].first;
        while (i < lines[l].last) {
            uint32_t colour = 0;
            int end = lines[l].last;
            for (int sp = 0; sp < c.span_count; sp++) {
                const int s0 = c.spans[sp].start, s1 = s0 + c.spans[sp].length;
                if (i >= s0 && i < s1) {
                    colour = c.spans[sp].colour;
                    end = std::min(end, s1);
                    break;
                }
                if (s0 > i) end = std::min(end, s0);
            }
            const ImU32 col = colour ? ImGui::GetColorU32(IM_COL32((colour >> 16) & 0xFF, (colour >> 8) & 0xFF,
                                                                   colour & 0xFF, 255))
                                     : Rgba(0.96f, 0.97f, 1.0f, 1.0f);
            dl->AddText(font, body_size, ImVec2(tx + body_size * 0.05f, ty + body_size * 0.05f),
                        Rgba(0, 0, 0, 0.6f), c.body + i, c.body + end);
            dl->AddText(font, body_size, ImVec2(tx, ty), col, c.body + i, c.body + end);
            tx += font->CalcTextSizeA(body_size, FLT_MAX, 0.0f, c.body + i, c.body + end).x;
            i = end;
        }
        ty += body_size * 1.22f;
    }
}

}  // namespace hud
