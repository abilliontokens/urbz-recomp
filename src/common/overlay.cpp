// the in-game overlay: F2 a performance panel, F1 a settings panel, both off until asked for.
// app::GameInfo::ui calls Draw on the render thread, inside the renderer's own ImGui frame and
// under app::UiLock (gpu/d3d12.cpp RenderUi), so nothing here may block, allocate per frame or
// trust that a context, a window size or a pad exists.
#include "common/overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <imgui.h>

#include "app/app.h"

namespace overlay {

namespace {

// the guest creates its device at 640x480 once and never resizes it (xdk/d3d8.cpp), so the host
// frame target is that, widened for ultrawide, times the render scale (gpu/d3d12.cpp
// CreateTargetResource); the inner rounding happens in guest pixels, before the scale
constexpr float kGuestW = 640.0f, kGuestH = 480.0f;

constexpr int kSamples = 120;  // frame times behind the graph, two seconds at 60 fps

// the frame time ring: written every frame whether or not the panel is up, so the graph is
// already full the moment F2 opens it
struct Ring {
    float ms[kSamples] = {};
    int next = 0;   // where the next sample goes, and the oldest one once the ring is full
    int count = 0;  // samples in the ring, up to kSamples
};
Ring g_ring;

bool g_perf_open = false, g_settings_open = false;
bool g_settings_focus = false;  // the settings window takes focus the frame it opens
float g_ui_scale = 0.0f;        // style and font scale in use, 0 before the first frame
// g_settings_open is the render thread's; this is the copy the window and guest threads read
std::atomic<bool> g_settings_up{false};

// the settings panel edits a copy and only writes it back on Apply, so a half typed resolution
// never reaches the renderer
struct Draft {
    app::DisplayMode mode = app::DisplayMode::Borderless;
    uint32_t width = 0, height = 0;
    char aspect[12] = "auto";
    bool ultrawide = true;
    bool vsync = true;
    uint32_t fps_cap = 60;
    int render_percent = 100;
    uint32_t aniso = 0;
};
Draft g_draft;
app::Mode g_desktop{0, 0};   // read when the panel opens: the mode lookup is a syscall
char g_status[80] = "";      // what the last Apply or Save did
float g_status_left = 0.0f;  // seconds it stays up

constexpr app::Mode kResolutions[] = {
    {1280, 720},  {1280, 800},  {1600, 900},  {1680, 1050}, {1920, 1080}, {1920, 1200},
    {2560, 1080}, {2560, 1440}, {2560, 1600}, {3440, 1440}, {3840, 1080}, {3840, 1600},
    {3840, 2160}, {5120, 1440},
};
constexpr uint32_t kFpsCaps[] = {0, 30, 60, 72, 120, 144, 165, 240};
constexpr uint32_t kAniso[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"As the game asks", "2x", "4x", "8x", "16x"};
const char* const kAspects[] = {"auto", "4:3", "16:9", "16:10", "21:9", "32:9"};
const char* const kModeNames[] = {"Fullscreen", "Borderless", "Windowed"};

// the dark style this overlay wears, built once at scale 1 and rescaled from it
const ImGuiStyle& BaseStyle() {
    static const ImGuiStyle base = [] {
        ImGuiStyle s;
        ImGui::StyleColorsDark(&s);
        const ImVec4 accent{0.29f, 0.56f, 0.78f, 1.00f};  // muted steel blue, not the ImGui blue
        const ImVec4 accent_hot{0.40f, 0.68f, 0.92f, 1.00f};
        ImVec4* c = s.Colors;
        c[ImGuiCol_Text] = {0.90f, 0.92f, 0.94f, 1.00f};
        c[ImGuiCol_TextDisabled] = {0.48f, 0.52f, 0.58f, 1.00f};
        c[ImGuiCol_WindowBg] = {0.06f, 0.07f, 0.09f, 0.96f};
        c[ImGuiCol_ChildBg] = {0.09f, 0.10f, 0.13f, 0.60f};
        c[ImGuiCol_PopupBg] = {0.08f, 0.09f, 0.11f, 0.98f};
        c[ImGuiCol_Border] = {1.00f, 1.00f, 1.00f, 0.08f};
        c[ImGuiCol_BorderShadow] = {0.00f, 0.00f, 0.00f, 0.00f};
        c[ImGuiCol_FrameBg] = {0.15f, 0.17f, 0.21f, 1.00f};
        c[ImGuiCol_FrameBgHovered] = {0.20f, 0.23f, 0.28f, 1.00f};
        c[ImGuiCol_FrameBgActive] = {0.24f, 0.28f, 0.34f, 1.00f};
        c[ImGuiCol_TitleBg] = {0.06f, 0.07f, 0.09f, 1.00f};
        c[ImGuiCol_TitleBgActive] = {0.10f, 0.12f, 0.15f, 1.00f};
        c[ImGuiCol_TitleBgCollapsed] = {0.06f, 0.07f, 0.09f, 0.80f};
        c[ImGuiCol_ScrollbarBg] = {0.00f, 0.00f, 0.00f, 0.00f};
        c[ImGuiCol_ScrollbarGrab] = {0.24f, 0.28f, 0.34f, 1.00f};
        c[ImGuiCol_ScrollbarGrabHovered] = {0.31f, 0.36f, 0.43f, 1.00f};
        c[ImGuiCol_ScrollbarGrabActive] = accent;
        c[ImGuiCol_CheckMark] = accent_hot;
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_SliderGrabActive] = accent_hot;
        c[ImGuiCol_Button] = {0.17f, 0.20f, 0.25f, 1.00f};
        c[ImGuiCol_ButtonHovered] = {0.24f, 0.33f, 0.43f, 1.00f};
        c[ImGuiCol_ButtonActive] = accent;
        c[ImGuiCol_Header] = {0.29f, 0.56f, 0.78f, 0.32f};
        c[ImGuiCol_HeaderHovered] = {0.29f, 0.56f, 0.78f, 0.55f};
        c[ImGuiCol_HeaderActive] = accent;
        c[ImGuiCol_Separator] = {1.00f, 1.00f, 1.00f, 0.10f};
        c[ImGuiCol_SeparatorHovered] = {0.29f, 0.56f, 0.78f, 0.55f};
        c[ImGuiCol_SeparatorActive] = accent;
        c[ImGuiCol_ResizeGrip] = {1.00f, 1.00f, 1.00f, 0.06f};
        c[ImGuiCol_ResizeGripHovered] = {0.29f, 0.56f, 0.78f, 0.55f};
        c[ImGuiCol_ResizeGripActive] = accent;
        c[ImGuiCol_PlotLines] = accent_hot;
        c[ImGuiCol_PlotLinesHovered] = accent_hot;
        c[ImGuiCol_TextSelectedBg] = {0.29f, 0.56f, 0.78f, 0.35f};
        c[ImGuiCol_NavHighlight] = accent;
        s.WindowPadding = {18.0f, 16.0f};
        s.FramePadding = {10.0f, 6.0f};
        s.ItemSpacing = {10.0f, 8.0f};
        s.ItemInnerSpacing = {8.0f, 6.0f};
        s.CellPadding = {8.0f, 5.0f};
        s.IndentSpacing = 18.0f;
        s.ScrollbarSize = 12.0f;
        s.GrabMinSize = 12.0f;
        s.WindowBorderSize = 1.0f;
        s.ChildBorderSize = 1.0f;
        s.PopupBorderSize = 1.0f;
        s.FrameBorderSize = 0.0f;
        s.WindowRounding = 9.0f;
        s.ChildRounding = 7.0f;
        s.FrameRounding = 5.0f;
        s.PopupRounding = 7.0f;
        s.ScrollbarRounding = 9.0f;
        s.GrabRounding = 5.0f;
        s.TabRounding = 6.0f;
        s.WindowTitleAlign = {0.5f, 0.5f};
        s.SeparatorTextBorderSize = 1.0f;
        s.SeparatorTextPadding = {0.0f, 10.0f};
        s.SeparatorTextAlign = {0.0f, 0.5f};
        s.AntiAliasedLines = true;
        s.AntiAliasedFill = true;
        return s;
    }();
    return base;
}

// readable on a 3840x2160 window instead of microscopic: everything grows with the output height,
// in quarter steps so the style is only rebuilt when the window really changes size
void ScaleStyle(ImGuiIO& io) {
    float want = std::clamp(std::round(io.DisplaySize.y / 1080.0f * 4.0f) / 4.0f, 1.0f, 4.0f);
    if (std::fabs(want - g_ui_scale) < 1e-3f) return;
    g_ui_scale = want;
    ImGui::GetStyle() = BaseStyle();
    ImGui::GetStyle().ScaleAllSizes(want);
    io.FontGlobalScale = want;  // ImGui picks the font size up at the next NewFrame
}

void Sample(float dt) {
    // a stall between presents (a load, an alt-tab) would otherwise flatten the graph for seconds
    g_ring.ms[g_ring.next] = std::clamp(dt * 1000.0f, 0.0f, 200.0f);
    g_ring.next = (g_ring.next + 1) % kSamples;
    if (g_ring.count < kSamples) g_ring.count++;
}

// the name of an aspect when it is one of the usual ones, else null
const char* AspectName(float a) {
    struct Named {
        float ratio;
        const char* name;
    };
    static constexpr Named kNamed[] = {
        {4.0f / 3.0f, "4:3"},    {16.0f / 10.0f, "16:10"}, {16.0f / 9.0f, "16:9"},
        {64.0f / 27.0f, "21:9"}, {32.0f / 9.0f, "32:9"},
    };
    for (const Named& n : kNamed)
        if (std::fabs(a - n.ratio) < 0.02f) return n.name;
    return nullptr;
}

// the host frame target for a render scale, exactly as CreateTargetResource builds it
void FrameTarget(float scale, float widen, uint32_t& w, uint32_t& h) {
    long guest_w = std::lround(kGuestW * widen);
    w = uint32_t(std::max<long>(std::lround(guest_w * scale), 1));
    h = uint32_t(std::max<long>(std::lround(kGuestH * scale), 1));
}

void DrawPerf(double fps) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& st = ImGui::GetStyle();
    // pinned to the top right corner, out of the way of anything the game puts on screen
    ImGui::SetNextWindowPos({io.DisplaySize.x - st.WindowPadding.x, st.WindowPadding.y}, ImGuiCond_Always,
                            {1.0f, 0.0f});
    ImGui::SetNextWindowSize({270.0f * g_ui_scale, 0.0f}, ImGuiCond_Always);  // height auto fits
    ImGui::SetNextWindowBgAlpha(0.45f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMouseInputs;
    if (!ImGui::Begin("##urbz_perf", nullptr, kFlags)) {
        ImGui::End();
        return;
    }

    float last = 0.0f, lo = 0.0f, hi = 0.0f, sum = 0.0f;
    if (g_ring.count > 0) {
        // the ring fills from 0, so entries below count are always the real samples
        lo = hi = g_ring.ms[0];
        for (int i = 0; i < g_ring.count; i++) {
            float v = g_ring.ms[i];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            sum += v;
        }
        last = g_ring.ms[(g_ring.next + kSamples - 1) % kSamples];
    }

    ImGui::PushStyleColor(ImGuiCol_Text, st.Colors[ImGuiCol_PlotLines]);
    ImGui::Text("%.1f fps", fps);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, st.ItemSpacing.x * 2.0f);
    ImGui::Text("%.2f ms", last);

    char graph[48];
    std::snprintf(graph, sizeof graph, "%.1f - %.1f ms, avg %.1f", lo, hi,
                  g_ring.count ? sum / float(g_ring.count) : 0.0f);
    // values_offset is the oldest sample: the write cursor once the ring has wrapped
    ImGui::PlotLines("##frametime", g_ring.ms, g_ring.count, g_ring.count == kSamples ? g_ring.next : 0, graph,
                     0.0f, hi * 1.25f + 1.0f, {ImGui::GetContentRegionAvail().x, 44.0f * g_ui_scale});

    float widen = app::AspectWiden();
    float scale = app::RenderScale(uint32_t(kGuestH));
    uint32_t rw = 0, rh = 0;
    FrameTarget(scale, widen, rw, rh);
    float aspect = app::Widescreen() ? 16.0f / 9.0f * widen : 4.0f / 3.0f;

    const float col = ImGui::CalcTextSize("render ").x;
    char value[48];
    auto row = [col, &value](const char* label) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(col);
        ImGui::TextUnformatted(value);
    };
    std::snprintf(value, sizeof value, "%.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
    row("host");
    std::snprintf(value, sizeof value, "%u x %u  (%u%%)", rw, rh, app::Get().render_percent);
    row("render");
    if (const char* name = AspectName(aspect)) std::snprintf(value, sizeof value, "%s  (%.2f)", name, aspect);
    else std::snprintf(value, sizeof value, "%.2f", aspect);
    row("aspect");

    ImGui::Spacing();
    ImGui::TextDisabled("F1 settings");
    ImGui::End();
}

void SyncDraft() {
    const app::Settings& s = app::Get();
    g_draft.mode = s.display_mode;
    g_draft.width = s.width;
    g_draft.height = s.height;
    std::snprintf(g_draft.aspect, sizeof g_draft.aspect, "%s", s.aspect_ratio.c_str());
    g_draft.ultrawide = s.ultrawide;
    g_draft.vsync = s.vsync;
    g_draft.fps_cap = s.fps_cap;
    g_draft.render_percent = int(s.render_percent);
    g_draft.aniso = s.aniso;
    g_desktop = app::Desktop();
    g_status[0] = '\0';
    g_status_left = 0.0f;
}

void Status(const char* text) {
    std::snprintf(g_status, sizeof g_status, "%s", text);
    g_status_left = 4.0f;
}

void Apply() {
    app::Settings& s = app::Get();
    // the renderer re-reads the settings every present (ApplySettings), so only the window itself
    // has to be told, and only when its mode or size actually moved
    bool relayout = s.display_mode != g_draft.mode || s.width != g_draft.width || s.height != g_draft.height;
    s.display_mode = g_draft.mode;
    s.width = g_draft.width;
    s.height = g_draft.height;
    // the guest threads read this setting too (app::Widescreen and friends): every choice is
    // short enough to live in the string's own storage, so writing it never moves a buffer
    s.aspect_ratio = g_draft.aspect;
    s.ultrawide = g_draft.ultrawide;
    s.vsync = g_draft.vsync;
    s.fps_cap = g_draft.fps_cap;
    s.render_percent = uint32_t(std::max(g_draft.render_percent, 1));
    s.aniso = g_draft.aniso;
    if (relayout) app::ApplyDisplay();
}

// the label column of a settings row, with the control stretched across the rest
void Row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// what a control really does, under it, wrapped to the column
void Note(const char* text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

bool BeginRows(const char* id, float label_width) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void DrawDisplay() {
    Row("Display mode");
    int mode = int(g_draft.mode);
    if (ImGui::Combo("##mode", &mode, kModeNames, int(std::size(kModeNames)))) g_draft.mode = app::DisplayMode(mode);

    Row("Resolution");
    char text[48];
    if (g_draft.width && g_draft.height) std::snprintf(text, sizeof text, "%u x %u", g_draft.width, g_draft.height);
    else std::snprintf(text, sizeof text, "Desktop (%u x %u)", g_desktop.width, g_desktop.height);
    if (ImGui::BeginCombo("##resolution", text)) {
        std::snprintf(text, sizeof text, "Desktop (%u x %u)", g_desktop.width, g_desktop.height);
        if (ImGui::Selectable(text, !g_draft.width || !g_draft.height)) g_draft.width = g_draft.height = 0;
        for (const app::Mode& m : kResolutions) {
            std::snprintf(text, sizeof text, "%u x %u", m.width, m.height);
            if (ImGui::Selectable(text, g_draft.width == m.width && g_draft.height == m.height)) {
                g_draft.width = m.width;
                g_draft.height = m.height;
            }
        }
        ImGui::EndCombo();
    }
    Note("Windowed sizes the window and fullscreen switches the display mode; borderless always "
         "fills the monitor and takes this as the render size alone.");

    Row("Aspect ratio");
    if (ImGui::BeginCombo("##aspect", g_draft.aspect)) {
        for (const char* a : kAspects)
            if (ImGui::Selectable(a, std::strcmp(a, g_draft.aspect) == 0))
                std::snprintf(g_draft.aspect, sizeof g_draft.aspect, "%s", a);
        ImGui::EndCombo();
    }
    Note("Auto follows the shape of the output itself.");

    Row("Ultrawide");
    ImGui::Checkbox("##ultrawide", &g_draft.ultrawide);
    Note("Widens the view on a display wider than 16:9, and does nothing on one that is not.");

    Row("Vertical sync");
    ImGui::Checkbox("##vsync", &g_draft.vsync);

    Row("Frame rate cap");
    if (g_draft.fps_cap) std::snprintf(text, sizeof text, "%u fps", g_draft.fps_cap);
    else std::snprintf(text, sizeof text, "Unlimited");
    if (ImGui::BeginCombo("##fpscap", text)) {
        for (uint32_t cap : kFpsCaps) {
            if (cap) std::snprintf(text, sizeof text, "%u fps", cap);
            else std::snprintf(text, sizeof text, "Unlimited");
            if (ImGui::Selectable(text, g_draft.fps_cap == cap)) g_draft.fps_cap = cap;
        }
        ImGui::EndCombo();
    }
    Note("Any of these runs the game at its real speed: the frame step is the time that really "
         "passed, not a 60th of a second per frame (src/game/hooks.cpp).");
}

void DrawQuality() {
    Row("Render scale");
    ImGui::SliderInt("##renderscale", &g_draft.render_percent, 50, 200, "%d %%");
    // the same arithmetic the renderer does: the output height, this share of it, over the guest's
    app::Mode out = g_draft.width && g_draft.height ? app::Mode{g_draft.width, g_draft.height} : g_desktop;
    float scale = std::max(out.height * (g_draft.render_percent / 100.0f) / kGuestH, 0.1f);
    uint32_t rw = 0, rh = 0;
    FrameTarget(scale, app::AspectWiden(), rw, rh);
    char text[80];
    std::snprintf(text, sizeof text, "%u x %u frame target, from the guest's 640 x 480.", rw, rh);
    Note(text);

    Row("Anisotropic filtering");
    int aniso = 0;
    for (int i = 0; i < int(std::size(kAniso)); i++)
        if (kAniso[i] == g_draft.aniso) aniso = i;
    if (ImGui::Combo("##aniso", &aniso, kAnisoNames, int(std::size(kAnisoNames)))) g_draft.aniso = kAniso[aniso];
    Note("Forced only where the game already filters across mip levels.");
}

void DrawSettings() {
    const ImGuiIO& io = ImGui::GetIO();
    // centred, auto fitting its height, and resizable once it is up
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({620.0f * g_ui_scale, 0.0f}, ImGuiCond_Appearing);
    if (g_settings_focus) ImGui::SetNextWindowFocus();
    g_settings_focus = false;
    bool open = true;
    if (!ImGui::Begin("Settings###urbz_settings", &open,
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) g_settings_open = false;
        return;
    }

    const float label_width = ImGui::CalcTextSize("Anisotropic filtering").x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SeparatorText("Display");
    if (BeginRows("##display_rows", label_width)) {
        DrawDisplay();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Quality");
    if (BeginRows("##quality_rows", label_width)) {
        DrawQuality();
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const float button = ImGui::GetFontSize() * 6.0f;
    if (ImGui::Button("Apply", {button, 0.0f})) {
        Apply();
        Status("Applied: the renderer takes it from the next frame.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save", {button, 0.0f})) {
        Apply();
        app::Save();
        Status("Applied and written to settings.toml.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", {button, 0.0f})) open = false;

    if (g_status_left > 0.0f) {
        g_status_left -= io.DeltaTime;
        ImGui::TextDisabled("%s", g_status);
    } else {
        ImGui::TextDisabled("F1 closes this panel, F2 shows the frame times.");
    }
    ImGui::End();
    if (!open) g_settings_open = false;
}

}  // namespace

void Draw(double fps) {
    // the renderer calls this inside its frame, but a hook that cannot be reached by a broken
    // context is worth the two compares: this runs on the render thread
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();

    // the pad belongs to the game alone. main.cpp turns gamepad navigation on for ImGui at
    // startup, and the Win32 backend feeds XInput into it whatever the flag says, so clear the
    // flag every frame: no overlay window can ever act on a button the player pressed
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;

    // minimized, or before the backend has a size: nothing to draw into
    if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) return;
    ScaleStyle(io);
    Sample(io.DeltaTime);

    // the window thread feeds every key message to ImGui under the same lock (main.cpp WndProc),
    // so these land while the game has focus. F3 is the frame capture's
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        g_settings_open = !g_settings_open;
        if (g_settings_open) {
            SyncDraft();
            g_settings_focus = true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) g_perf_open = !g_perf_open;
    // the keyboard-to-pad layer stands down while the panel has the keyboard (input_kbm.cpp)
    g_settings_up.store(g_settings_open, std::memory_order_relaxed);

    // the mouse is only ImGui's while the settings panel is up; closed, the overlay neither
    // captures a click nor touches the cursor, and io.WantCaptureMouse stays false
    if (g_settings_open) io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;

    if (!g_perf_open && !g_settings_open) return;  // closed: the overlay draws nothing at all
    if (g_perf_open) DrawPerf(fps);
    if (g_settings_open) DrawSettings();
}

bool SettingsOpen() { return g_settings_up.load(std::memory_order_relaxed); }

}  // namespace overlay
