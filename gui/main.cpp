// Graphical front-end for the HEIC -> JPG converter.
//
// Dear ImGui + GLFW + OpenGL3. Drag-and-drop or browse for .heic photos, set
// quality / threads / output folder, hit Convert, and watch an animated
// progress view. The actual conversion is done by converter_core (shared with
// the CLI) on a background thread.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>
#include <nfd.h>

#include "converter_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/stat.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <shellapi.h>   // ShellExecuteA
#endif

// ---------------------------------------------------------------------------
// Small math / easing helpers
// ---------------------------------------------------------------------------
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float smoothstep(float x) { x = clampf(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); }
// Frame-rate independent approach toward a target.
static float approach(float cur, float target, float speed, float dt) {
    return cur + (target - cur) * (1.f - std::exp(-speed * dt));
}
static ImVec4 approach(const ImVec4& c, const ImVec4& t, float speed, float dt) {
    return ImVec4(approach(c.x, t.x, speed, dt), approach(c.y, t.y, speed, dt),
                  approach(c.z, t.z, speed, dt), approach(c.w, t.w, speed, dt));
}
static ImVec4 withAlpha(ImVec4 c, float a) { c.w = a; return c; }
static ImU32 col32(const ImVec4& c, float am = 1.f) { return ImGui::GetColorU32(withAlpha(c, c.w * am)); }

// ---------------------------------------------------------------------------
// Themes
// ---------------------------------------------------------------------------
struct Theme {
    const char* name;
    ImVec4 bg0, bg1;     // background gradient (top, bottom)
    ImVec4 glowA, glowB; // aurora blob colors
    ImVec4 panel;        // card / panel fill
    ImVec4 accent, accent2;
    ImVec4 text, textDim;
    ImVec4 good, bad;
};

static const Theme kThemes[] = {
    {"Midnight",
     {0.06f,0.07f,0.12f,1}, {0.02f,0.02f,0.05f,1},
     {0.27f,0.36f,0.92f,1}, {0.66f,0.28f,0.86f,1},
     {0.13f,0.15f,0.22f,0.85f},
     {0.40f,0.55f,1.00f,1}, {0.66f,0.42f,1.00f,1},
     {0.92f,0.94f,1.00f,1}, {0.58f,0.62f,0.78f,1},
     {0.35f,0.85f,0.55f,1}, {0.96f,0.42f,0.45f,1}},
    {"Aurora",
     {0.03f,0.11f,0.12f,1}, {0.02f,0.05f,0.09f,1},
     {0.20f,0.85f,0.70f,1}, {0.30f,0.55f,0.95f,1},
     {0.09f,0.17f,0.19f,0.85f},
     {0.20f,0.86f,0.66f,1}, {0.36f,0.68f,0.98f,1},
     {0.90f,0.98f,0.96f,1}, {0.55f,0.72f,0.70f,1},
     {0.40f,0.90f,0.60f,1}, {0.98f,0.46f,0.50f,1}},
    {"Sunset",
     {0.14f,0.07f,0.12f,1}, {0.06f,0.03f,0.07f,1},
     {0.98f,0.45f,0.35f,1}, {0.95f,0.30f,0.55f,1},
     {0.20f,0.11f,0.16f,0.85f},
     {0.99f,0.55f,0.36f,1}, {0.98f,0.36f,0.56f,1},
     {1.00f,0.96f,0.93f,1}, {0.80f,0.62f,0.64f,1},
     {0.55f,0.86f,0.55f,1}, {0.99f,0.40f,0.42f,1}},
    {"Forest",
     {0.07f,0.10f,0.08f,1}, {0.03f,0.05f,0.04f,1},
     {0.40f,0.72f,0.34f,1}, {0.78f,0.74f,0.30f,1},
     {0.12f,0.16f,0.13f,0.85f},
     {0.52f,0.80f,0.38f,1}, {0.86f,0.78f,0.36f,1},
     {0.93f,0.97f,0.90f,1}, {0.62f,0.72f,0.60f,1},
     {0.48f,0.86f,0.50f,1}, {0.95f,0.46f,0.40f,1}},
    {"Daylight",
     {0.92f,0.94f,0.98f,1}, {0.82f,0.86f,0.93f,1},
     {0.45f,0.60f,0.98f,1}, {0.60f,0.42f,0.95f,1},
     {1.00f,1.00f,1.00f,0.80f},
     {0.27f,0.45f,0.95f,1}, {0.52f,0.34f,0.92f,1},
     {0.10f,0.13f,0.20f,1}, {0.38f,0.42f,0.52f,1},
     {0.18f,0.66f,0.38f,1}, {0.86f,0.26f,0.30f,1}},
};
static const int kThemeCount = (int)(sizeof(kThemes) / sizeof(kThemes[0]));

// "Live" theme whose colors are lerped toward the selected theme for smooth
// transitions when the user switches.
struct LiveTheme {
    Theme t = kThemes[0];
    void approachTo(const Theme& target, float dt) {
        float s = 9.f;
        t.bg0 = approach(t.bg0, target.bg0, s, dt);
        t.bg1 = approach(t.bg1, target.bg1, s, dt);
        t.glowA = approach(t.glowA, target.glowA, s, dt);
        t.glowB = approach(t.glowB, target.glowB, s, dt);
        t.panel = approach(t.panel, target.panel, s, dt);
        t.accent = approach(t.accent, target.accent, s, dt);
        t.accent2 = approach(t.accent2, target.accent2, s, dt);
        t.text = approach(t.text, target.text, s, dt);
        t.textDim = approach(t.textDim, target.textDim, s, dt);
        t.good = approach(t.good, target.good, s, dt);
        t.bad = approach(t.bad, target.bad, s, dt);
        t.name = target.name;
    }
};

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
enum class FileStatus { Pending, Working, Done, Failed };

struct FileItem {
    std::string path;
    std::string name;
    FileStatus  status = FileStatus::Pending;
    std::string err;
    float anim = 0.f;       // entry animation 0..1
    float statusAnim = 0.f; // status-change pop 0..1
};

struct Particle {
    ImVec2 pos, vel;
    ImVec4 color;
    float life, maxLife, size, rot, spin;
};

struct Toast {
    std::string text;
    float life;
    ImVec4 color;
};

struct App {
    std::vector<FileItem> files;
    std::unordered_set<std::string> known;   // dedupe
    std::string outputDir;

    int quality = 90;
    int threads = 4;

    int themeIndex = 0;
    LiveTheme live;

    // conversion
    hjc::Progress progress;
    std::thread worker;
    bool converting = false;
    bool justFinished = false;
    float finishFlash = 0.f;

    std::mutex resultMtx;
    std::vector<std::pair<std::string, std::pair<bool, std::string>>> results; // path -> (ok, err)

    // visuals
    float dropFlash = 0.f;
    float progressShown = 0.f;     // smoothed progress fraction
    std::vector<Particle> confetti;
    std::deque<Toast> toasts;

    std::unordered_map<ImGuiID, float> hoverAnim; // per-widget hover state
    std::mt19937 rng{std::random_device{}()};

    ImFont* fontBig = nullptr;
    ImFont* fontH = nullptr;

    void addPath(const std::string& p);
    void pushToast(const std::string& s, ImVec4 c) { toasts.push_back({s, 4.0f, c}); }
};

static std::string parentDir(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
}

void App::addPath(const std::string& p) {
    // Directory? pull every .heic out of it.
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(p.c_str());
    bool isDir = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    bool isDir = (stat(p.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
#endif
    if (isDir) {
        for (auto& f : hjc::list_heic_files(p)) addPath(f);
        return;
    }
    if (!hjc::ends_with_heic_ci(p)) return;
    if (known.count(p)) return;
    known.insert(p);
    FileItem it;
    it.path = p;
    it.name = hjc::path_basename(p);
    files.push_back(std::move(it));
    if (outputDir.empty()) outputDir = parentDir(p) + "/converted";
}

static void openFolder(const std::string& path) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + path + "\"";
    std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + path + "\"";
    std::system(cmd.c_str());
#endif
}

// ---------------------------------------------------------------------------
// GLFW drop callback
// ---------------------------------------------------------------------------
static void dropCallback(GLFWwindow* w, int count, const char** paths) {
    App* app = (App*)glfwGetWindowUserPointer(w);
    if (!app) return;
    int before = (int)app->files.size();
    for (int i = 0; i < count; i++) app->addPath(paths[i]);
    int added = (int)app->files.size() - before;
    app->dropFlash = 1.0f;
    if (added > 0) app->pushToast(std::to_string(added) + " photo" + (added == 1 ? "" : "s") + " added",
                                  app->live.t.good);
    else app->pushToast("No .heic photos found", app->live.t.bad);
}

// ---------------------------------------------------------------------------
// Custom drawing helpers
// ---------------------------------------------------------------------------
static float& hoverOf(App& app, ImGuiID id) {
    auto it = app.hoverAnim.find(id);
    if (it == app.hoverAnim.end()) it = app.hoverAnim.emplace(id, 0.f).first;
    return it->second;
}

// A rounded, animated button drawn by hand. Returns true when clicked.
static bool fancyButton(App& app, const char* label, ImVec2 size,
                        ImVec4 accent, bool enabled = true, bool filled = true) {
    ImGui::PushID(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGuiID id = ImGui::GetID("btn");
    if (!enabled) ImGui::BeginDisabled();
    bool clicked = ImGui::InvisibleButton("btn", size);
    if (!enabled) ImGui::EndDisabled();

    bool hovered = enabled && ImGui::IsItemHovered();
    bool held = enabled && ImGui::IsItemActive();
    float& h = hoverOf(app, id);
    float dt = ImGui::GetIO().DeltaTime;
    h = approach(h, hovered ? 1.f : 0.f, 14.f, dt);

    float press = held ? 0.96f : 1.f;
    ImVec2 c = ImVec2(p.x + size.x * 0.5f, p.y + size.y * 0.5f);
    ImVec2 half = ImVec2(size.x * 0.5f * press, size.y * 0.5f * press);
    ImVec2 a = ImVec2(c.x - half.x, c.y - half.y);
    ImVec2 b = ImVec2(c.x + half.x, c.y + half.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rounding = size.y * 0.28f;
    float alpha = enabled ? 1.f : 0.4f;

    if (filled) {
        ImVec4 top = accent;
        ImVec4 bot = app.live.t.accent2;
        top.w = alpha; bot.w = alpha;
        // glow
        if (h > 0.01f) {
            float g = h * (0.45f + 0.15f * sinf((float)ImGui::GetTime() * 4.f));
            dl->AddRectFilled(ImVec2(a.x - 8, a.y - 8), ImVec2(b.x + 8, b.y + 8),
                              col32(withAlpha(accent, 0.35f * g)), rounding + 8);
        }
        dl->AddRectFilledMultiColor(a, b,
            col32(ImVec4(top.x + 0.06f, top.y + 0.06f, top.z + 0.06f, alpha)),
            col32(bot),
            col32(bot),
            col32(ImVec4(top.x + 0.06f, top.y + 0.06f, top.z + 0.06f, alpha)));
        dl->AddRect(a, b, col32(withAlpha(ImVec4(1,1,1,1), 0.18f + 0.20f * h)), rounding, 0, 1.5f);
    } else {
        ImVec4 fill = app.live.t.panel;
        fill.w *= (0.5f + 0.5f * h) * alpha;
        dl->AddRectFilled(a, b, col32(fill), rounding);
        dl->AddRect(a, b, col32(withAlpha(accent, (0.4f + 0.5f * h) * alpha)), rounding, 0, 1.5f);
    }

    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec4 tc = filled ? ImVec4(1,1,1,alpha) : withAlpha(app.live.t.text, alpha);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col32(tc), label);
    ImGui::PopID();
    return clicked;
}

static void drawSpinner(ImDrawList* dl, ImVec2 center, float radius, float t, ImU32 col) {
    const int n = 30;
    float base = t * 5.5f;
    dl->PathClear();
    for (int i = 0; i <= n; i++) {
        float a = base + (i / (float)n) * 4.2f;
        dl->PathLineTo(ImVec2(center.x + cosf(a) * radius, center.y + sinf(a) * radius));
    }
    dl->PathStroke(col, 0, 2.5f);
}

static void drawCheck(ImDrawList* dl, ImVec2 c, float s, float p, ImU32 col) {
    p = smoothstep(p);
    ImVec2 a(c.x - s * 0.45f, c.y + s * 0.02f);
    ImVec2 b(c.x - s * 0.12f, c.y + s * 0.32f);
    ImVec2 d(c.x + s * 0.48f, c.y - s * 0.34f);
    if (p < 0.5f) {
        float k = p / 0.5f;
        dl->AddLine(a, ImVec2(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k), col, 2.6f);
    } else {
        float k = (p - 0.5f) / 0.5f;
        dl->AddLine(a, b, col, 2.6f);
        dl->AddLine(b, ImVec2(b.x + (d.x - b.x) * k, b.y + (d.y - b.y) * k), col, 2.6f);
    }
}

static void drawCross(ImDrawList* dl, ImVec2 c, float s, float p, ImU32 col) {
    p = smoothstep(p);
    float r = s * 0.38f * p;
    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 2.6f);
    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, 2.6f);
}

// Marching-ants dashed rounded rect.
static void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                       ImU32 col, float thick, float phase) {
    dl->PathClear();
    dl->PathRect(a, b, rounding);
    const ImVector<ImVec2>& pts = dl->_Path;
    // Walk the path adding dashes.
    float dash = 11.f, gap = 8.f, dist = phase;
    std::vector<ImVec2> path(pts.begin(), pts.end());
    dl->PathClear();
    for (size_t i = 0; i + 1 < path.size(); i++) {
        ImVec2 p0 = path[i], p1 = path[i + 1];
        ImVec2 d(p1.x - p0.x, p1.y - p0.y);
        float len = sqrtf(d.x * d.x + d.y * d.y);
        if (len < 0.001f) continue;
        ImVec2 dir(d.x / len, d.y / len);
        float t = -fmodf(dist, dash + gap);
        while (t < len) {
            float s0 = clampf(t, 0.f, len);
            float s1 = clampf(t + dash, 0.f, len);
            if (s1 > s0)
                dl->AddLine(ImVec2(p0.x + dir.x * s0, p0.y + dir.y * s0),
                            ImVec2(p0.x + dir.x * s1, p0.y + dir.y * s1), col, thick);
            t += dash + gap;
        }
        dist += len;
    }
}

static void spawnConfetti(App& app, ImVec2 origin, int count) {
    std::uniform_real_distribution<float> ang(-3.14159f, 0.f);
    std::uniform_real_distribution<float> spd(180.f, 620.f);
    std::uniform_real_distribution<float> sz(3.f, 8.f);
    std::uniform_real_distribution<float> sp(-8.f, 8.f);
    ImVec4 palette[5] = {app.live.t.accent, app.live.t.accent2, app.live.t.good,
                         app.live.t.glowA, app.live.t.glowB};
    std::uniform_int_distribution<int> pick(0, 4);
    for (int i = 0; i < count; i++) {
        float a = ang(app.rng), s = spd(app.rng);
        Particle pt;
        pt.pos = origin;
        pt.vel = ImVec2(cosf(a) * s, sinf(a) * s);
        pt.color = palette[pick(app.rng)];
        pt.maxLife = pt.life = 1.4f + (s / 620.f);
        pt.size = sz(app.rng);
        pt.rot = ang(app.rng);
        pt.spin = sp(app.rng);
        app.confetti.push_back(pt);
    }
}

// ---------------------------------------------------------------------------
// Conversion control
// ---------------------------------------------------------------------------
static void startConversion(App& app) {
    if (app.converting || app.files.empty()) return;
    if (app.outputDir.empty()) app.outputDir = "converted";

    for (auto& f : app.files) {
        f.status = FileStatus::Pending;
        f.statusAnim = 0.f;
        f.err.clear();
    }
    { std::lock_guard<std::mutex> lk(app.resultMtx); app.results.clear(); }

    std::vector<std::string> paths;
    paths.reserve(app.files.size());
    for (auto& f : app.files) paths.push_back(f.path);

    app.converting = true;
    app.justFinished = false;
    app.progressShown = 0.f;

    std::string outDir = app.outputDir;
    int q = app.quality, t = app.threads;
    App* ap = &app;
    app.worker = std::thread([ap, paths, outDir, q, t]() {
        hjc::convert_files(paths, outDir, q, t, ap->progress,
            [ap](const std::string& path, bool ok, const std::string& err) {
                std::lock_guard<std::mutex> lk(ap->resultMtx);
                ap->results.push_back({path, {ok, err}});
            });
    });
}

static void pollConversion(App& app) {
    // Mark "working": files not yet reported are in-flight once running.
    std::vector<std::pair<std::string, std::pair<bool, std::string>>> drained;
    {
        std::lock_guard<std::mutex> lk(app.resultMtx);
        drained.swap(app.results);
    }
    for (auto& r : drained) {
        for (auto& f : app.files) {
            if (f.path == r.first) {
                f.status = r.second.first ? FileStatus::Done : FileStatus::Failed;
                f.err = r.second.second;
                f.statusAnim = 0.f;
                break;
            }
        }
    }
    if (app.converting && !app.progress.running.load()) {
        if (app.worker.joinable()) app.worker.join();
        app.converting = false;
        app.justFinished = true;
        app.finishFlash = 1.0f;
        int ok = app.progress.ok.load(), fail = app.progress.failed.load();
        if (fail == 0)
            app.pushToast("Done! " + std::to_string(ok) + " converted", app.live.t.good);
        else
            app.pushToast(std::to_string(ok) + " ok, " + std::to_string(fail) + " failed",
                          app.live.t.bad);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
static ImFont* tryLoadFont(ImGuiIO& io, const char* path, float px) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return nullptr;
    std::fclose(f);
    return io.Fonts->AddFontFromFileTTF(path, px);
}

int main(int, char**) {
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to init GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(980, 720, "HEIC to JPG Converter", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    App app;
    app.threads = (int)std::max(1u, std::thread::hardware_concurrency());
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, dropCallback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // ImGui lays out in logical (DPI-independent) coordinates; the GLFW backend
    // handles framebuffer scaling. (High-DPI crispness is a future polish item.)
    float ui = 1.0f;

    // Fonts: prefer a clean system UI font, fall back to ImGui's built-in.
    const char* fontPaths[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/segoeui.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
    };
    ImFont* base = nullptr;
    for (const char* fp : fontPaths) { base = tryLoadFont(io, fp, 18.f * ui); if (base) break; }
    if (!base) io.Fonts->AddFontDefault();
    for (const char* fp : fontPaths) { app.fontH = tryLoadFont(io, fp, 24.f * ui); if (app.fontH) break; }
    for (const char* fp : fontPaths) { app.fontBig = tryLoadFont(io, fp, 40.f * ui); if (app.fontBig) break; }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.f;
    style.FrameRounding = 8.f;
    style.GrabRounding = 8.f;
    style.ScrollbarRounding = 8.f;
    style.FrameBorderSize = 0.f;
    style.ScaleAllSizes(ui);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    NFD_Init();

    double last = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        double nowd = glfwGetTime();
        float dt = (float)(nowd - last);
        last = nowd;
        if (dt > 0.1f) dt = 0.1f;
        float now = (float)nowd;

        app.live.approachTo(kThemes[app.themeIndex], dt);
        app.dropFlash = approach(app.dropFlash, 0.f, 4.f, dt);
        app.finishFlash = approach(app.finishFlash, 0.f, 2.f, dt);
        pollConversion(app);

        const Theme& T = app.live.t;

        // Apply theme to ImGui controls
        ImVec4* c = style.Colors;
        c[ImGuiCol_Text] = T.text;
        c[ImGuiCol_TextDisabled] = T.textDim;
        c[ImGuiCol_FrameBg] = withAlpha(T.panel, 0.55f);
        c[ImGuiCol_FrameBgHovered] = withAlpha(T.accent, 0.30f);
        c[ImGuiCol_FrameBgActive] = withAlpha(T.accent, 0.45f);
        c[ImGuiCol_SliderGrab] = T.accent;
        c[ImGuiCol_SliderGrabActive] = T.accent2;
        c[ImGuiCol_CheckMark] = T.accent;
        c[ImGuiCol_ScrollbarBg] = ImVec4(0,0,0,0);
        c[ImGuiCol_ScrollbarGrab] = withAlpha(T.text, 0.18f);
        c[ImGuiCol_ScrollbarGrabHovered] = withAlpha(T.text, 0.30f);
        c[ImGuiCol_PopupBg] = withAlpha(T.bg1, 0.98f);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 size = vp->Size;
        ImDrawList* bg = ImGui::GetBackgroundDrawList();

        // --- animated background gradient + drifting aurora blobs ---
        bg->AddRectFilledMultiColor(ImVec2(0, 0), size,
            col32(T.bg0), col32(T.bg0), col32(T.bg1), col32(T.bg1));
        for (int i = 0; i < 3; i++) {
            float ph = now * (0.10f + 0.04f * i) + i * 2.1f;
            ImVec2 ctr(size.x * (0.5f + 0.42f * sinf(ph)),
                       size.y * (0.35f + 0.30f * cosf(ph * 0.8f + i)));
            ImVec4 gc = (i % 2 == 0) ? T.glowA : T.glowB;
            float rad = size.x * (0.34f + 0.05f * sinf(ph * 1.3f));
            const int rings = 5;
            for (int r = rings; r >= 1; r--) {
                float rr = rad * r / rings;
                bg->AddCircleFilled(ctr, rr, col32(withAlpha(gc, 0.05f)), 48);
            }
        }

        // --- main UI window ---
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
        ImGui::Begin("##main", nullptr, flags);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float pad = 28.f * ui;
        ImGui::SetCursorPos(ImVec2(pad, pad));

        // Header
        ImGui::PushFont(app.fontBig ? app.fontBig : base);
        ImGui::TextColored(T.text, "HEIC");
        ImGui::SameLine(0, 0);
        ImGui::TextColored(T.accent, " -> JPG");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(T.textDim, "  converter");

        // Theme switcher (top-right segmented control)
        {
            float bw = 92.f * ui, bh = 30.f * ui, gap = 8.f * ui;
            float totalw = kThemeCount * bw + (kThemeCount - 1) * gap;
            float x = size.x - pad - totalw;   // window-local X (window fills the viewport)
            float y = pad + 6.f * ui;
            for (int i = 0; i < kThemeCount; i++) {
                ImGui::SetCursorPos(ImVec2(x + i * (bw + gap), y));
                bool sel = (i == app.themeIndex);
                if (fancyButton(app, kThemes[i].name, ImVec2(bw, bh),
                                kThemes[i].accent, true, sel))
                    app.themeIndex = i;
            }
        }

        ImGui::SetCursorPos(ImVec2(pad, pad + 70.f * ui));

        // --- Drop zone ---
        float dzH = 168.f * ui;
        ImVec2 dzP = ImGui::GetCursorScreenPos();
        ImVec2 dzA = dzP;
        ImVec2 dzB = ImVec2(dzP.x + size.x - pad * 2, dzP.y + dzH);
        float flash = app.dropFlash;
        ImVec4 dzFill = withAlpha(T.panel, T.panel.w * (0.5f + 0.4f * flash));
        dl->AddRectFilled(dzA, dzB, col32(dzFill), 18.f * ui);
        float pulse = 0.5f + 0.5f * sinf(now * 2.2f);
        ImU32 dash = col32(withAlpha(T.accent, 0.55f + 0.35f * pulse + 0.4f * flash));
        dashedRect(dl, dzA, dzB, 18.f * ui, dash, 2.2f, now * 36.f);
        // cloud/arrow glyph
        ImVec2 gc(dzP.x + (dzB.x - dzA.x) * 0.5f, dzP.y + dzH * 0.40f);
        float gs = 30.f * ui * (1.f + 0.04f * sinf(now * 3.f));
        dl->AddCircleFilled(ImVec2(gc.x - gs * 0.5f, gc.y), gs * 0.5f, col32(withAlpha(T.accent, 0.25f)), 32);
        dl->AddCircleFilled(ImVec2(gc.x + gs * 0.5f, gc.y), gs * 0.55f, col32(withAlpha(T.accent, 0.25f)), 32);
        dl->AddRectFilled(ImVec2(gc.x - gs * 0.6f, gc.y), ImVec2(gc.x + gs * 0.7f, gc.y + gs * 0.5f),
                          col32(withAlpha(T.accent, 0.25f)), 6.f);
        dl->AddLine(ImVec2(gc.x, gc.y - gs * 0.2f), ImVec2(gc.x, gc.y - gs * 0.9f), col32(T.accent), 3.f);
        dl->AddTriangleFilled(ImVec2(gc.x - gs * 0.28f, gc.y - gs * 0.62f),
                              ImVec2(gc.x + gs * 0.28f, gc.y - gs * 0.62f),
                              ImVec2(gc.x, gc.y - gs * 1.05f), col32(T.accent));
        // text
        const char* dzText = "Drag & drop your HEIC photos here";
        ImGui::PushFont(app.fontH ? app.fontH : base);
        ImVec2 ts = ImGui::CalcTextSize(dzText);
        dl->AddText(ImVec2(gc.x - ts.x * 0.5f, dzP.y + dzH * 0.55f), col32(T.text), dzText);
        ImGui::PopFont();
        // Browse button inside drop zone
        {
            float bw = 150.f * ui, bh = 38.f * ui;
            ImGui::SetCursorScreenPos(ImVec2(gc.x - bw * 0.5f, dzP.y + dzH * 0.74f));
            if (fancyButton(app, "Browse files...", ImVec2(bw, bh), T.accent)) {
                nfdu8filteritem_t filt[1] = {{"HEIC images", "heic"}};
                const nfdpathset_t* set;
                if (NFD_OpenDialogMultipleU8(&set, filt, 1, nullptr) == NFD_OKAY) {
                    nfdpathsetsize_t n = 0; NFD_PathSet_GetCount(set, &n);
                    for (nfdpathsetsize_t i = 0; i < n; i++) {
                        nfdu8char_t* pth = nullptr;
                        if (NFD_PathSet_GetPathU8(set, i, &pth) == NFD_OKAY && pth) {
                            app.addPath(pth);
                            NFD_PathSet_FreePathU8(pth);
                        }
                    }
                    NFD_PathSet_Free(set);
                }
            }
        }
        // make the whole drop zone clickable to browse too
        ImGui::SetCursorScreenPos(dzA);

        ImGui::SetCursorPos(ImVec2(pad, pad + 70.f * ui + dzH + 22.f * ui));

        // --- File list ---
        float listH = size.y - (pad + 70.f * ui + dzH + 22.f * ui) - 188.f * ui;
        if (listH < 90.f * ui) listH = 90.f * ui;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, withAlpha(T.panel, T.panel.w * 0.45f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 16.f * ui);
        ImGui::BeginChild("filelist", ImVec2(size.x - pad * 2, listH), true);
        if (app.files.empty()) {
            ImVec2 av = ImGui::GetContentRegionAvail();
            ImGui::Dummy(ImVec2(0, av.y * 0.5f - 14.f * ui));
            const char* empty = "No photos yet - drop some in or click Browse.";
            float w = ImGui::CalcTextSize(empty).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - w) * 0.5f);
            ImGui::TextColored(T.textDim, "%s", empty);
        } else {
            int removeIdx = -1;
            ImDrawList* ldl = ImGui::GetWindowDrawList();
            for (int i = 0; i < (int)app.files.size(); i++) {
                FileItem& f = app.files[i];
                f.anim = approach(f.anim, 1.f, 10.f, dt);
                f.statusAnim = approach(f.statusAnim, 1.f, 8.f, dt);
                float rowH = 46.f * ui;
                ImVec2 rp = ImGui::GetCursorScreenPos();
                float slide = (1.f - smoothstep(f.anim)) * 30.f * ui;
                rp.x += slide;
                ImU32 rowCol = col32(withAlpha(T.text, 0.04f + 0.04f * (i & 1)));
                ldl->AddRectFilled(rp, ImVec2(rp.x + ImGui::GetContentRegionAvail().x - 6.f * ui, rp.y + rowH - 6.f * ui),
                                   rowCol, 10.f * ui);
                // status icon
                ImVec2 ic(rp.x + 24.f * ui, rp.y + rowH * 0.5f - 3.f * ui);
                float ir = 11.f * ui;
                switch (f.status) {
                    case FileStatus::Pending:
                        if (app.converting) drawSpinner(ldl, ic, ir, now, col32(withAlpha(T.accent, 0.55f)));
                        else ldl->AddCircle(ic, ir, col32(withAlpha(T.textDim, 0.6f)), 24, 2.f);
                        break;
                    case FileStatus::Working:
                        drawSpinner(ldl, ic, ir, now, col32(T.accent));
                        break;
                    case FileStatus::Done:
                        ldl->AddCircleFilled(ic, ir * (0.7f + 0.5f * smoothstep(f.statusAnim)),
                                             col32(withAlpha(T.good, 0.22f)), 24);
                        drawCheck(ldl, ic, ir * 1.7f, f.statusAnim, col32(T.good));
                        break;
                    case FileStatus::Failed:
                        ldl->AddCircleFilled(ic, ir, col32(withAlpha(T.bad, 0.22f)), 24);
                        drawCross(ldl, ic, ir * 1.7f, f.statusAnim, col32(T.bad));
                        break;
                }
                float tx = (1.f - smoothstep(f.anim));
                ImU32 nameCol = col32(withAlpha(T.text, (1.f - tx)));
                ldl->AddText(ImVec2(rp.x + 48.f * ui, rp.y + rowH * 0.5f - 9.f * ui), nameCol, f.name.c_str());
                if (f.status == FileStatus::Failed && !f.err.empty()) {
                    float ew = ImGui::CalcTextSize(f.err.c_str()).x;
                    ldl->AddText(ImVec2(rp.x + ImGui::GetContentRegionAvail().x - ew - 40.f * ui,
                                        rp.y + rowH * 0.5f - 9.f * ui),
                                 col32(withAlpha(T.bad, 0.9f)), f.err.c_str());
                }
                // remove button (x) when idle
                if (!app.converting) {
                    ImGui::SetCursorScreenPos(ImVec2(rp.x + ImGui::GetContentRegionAvail().x - 34.f * ui,
                                                     rp.y + rowH * 0.5f - 11.f * ui));
                    ImGui::PushID(i);
                    if (ImGui::InvisibleButton("rm", ImVec2(22.f * ui, 22.f * ui))) removeIdx = i;
                    bool hov = ImGui::IsItemHovered();
                    ImVec2 xc = ImGui::GetItemRectMin();
                    xc = ImVec2(xc.x + 11.f * ui, xc.y + 11.f * ui);
                    drawCross(ldl, xc, 14.f * ui, 1.f, col32(withAlpha(T.textDim, hov ? 1.f : 0.5f)));
                    ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(ImVec2(rp.x - slide, rp.y));
                ImGui::Dummy(ImVec2(0, rowH));
            }
            if (removeIdx >= 0) {
                app.known.erase(app.files[removeIdx].path);
                app.files.erase(app.files.begin() + removeIdx);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // --- Controls + Convert ---
        ImGui::SetCursorPos(ImVec2(pad, size.y - 150.f * ui));
        float colw = (size.x - pad * 2 - 24.f * ui) * 0.5f;

        ImGui::BeginGroup();
        ImGui::PushItemWidth(colw);
        ImGui::TextColored(T.textDim, "Quality");
        ImGui::SameLine();
        ImVec4 qc = app.quality >= 85 ? T.good : (app.quality >= 60 ? T.accent : T.bad);
        ImGui::TextColored(qc, "%d", app.quality);
        ImGui::SliderInt("##q", &app.quality, 1, 100, "");
        ImGui::TextColored(T.textDim, "Threads");
        ImGui::SameLine();
        ImGui::TextColored(T.text, "%d", app.threads);
        int maxT = (int)std::max(1u, std::thread::hardware_concurrency()) * 2;
        ImGui::SliderInt("##t", &app.threads, 1, std::max(2, maxT), "");
        ImGui::PopItemWidth();
        ImGui::EndGroup();

        ImGui::SameLine(0, 24.f * ui);

        ImGui::BeginGroup();
        ImGui::TextColored(T.textDim, "Output folder");
        std::string shown = app.outputDir.empty() ? "(beside your photos)" : app.outputDir;
        ImGui::PushFont(base);
        ImGui::PushStyleColor(ImGuiCol_Text, withAlpha(T.text, 0.85f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + colw);
        ImGui::TextWrapped("%s", shown.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (fancyButton(app, "Change folder...", ImVec2(160.f * ui, 34.f * ui), T.accent2, !app.converting, false)) {
            nfdu8char_t* out = nullptr;
            const char* def = app.outputDir.empty() ? nullptr : app.outputDir.c_str();
            if (NFD_PickFolderU8(&out, def) == NFD_OKAY && out) {
                app.outputDir = out;
                NFD_FreePathU8(out);
            }
        }
        ImGui::EndGroup();

        // Convert / progress bar
        float total = (float)app.progress.total.load();
        float done = (float)app.progress.done.load();
        float frac = total > 0 ? done / total : 0.f;
        app.progressShown = approach(app.progressShown, frac, 10.f, dt);

        ImVec2 cbP = ImVec2(vp->Pos.x + pad, vp->Pos.y + size.y - 70.f * ui);
        ImVec2 cbSz = ImVec2(size.x - pad * 2, 50.f * ui);
        ImGui::SetCursorScreenPos(cbP);

        if (!app.converting) {
            bool can = !app.files.empty();
            char label[64];
            std::snprintf(label, sizeof(label), "Convert %d photo%s",
                          (int)app.files.size(), app.files.size() == 1 ? "" : "s");
            if (fancyButton(app, app.files.empty() ? "Convert" : label, cbSz, T.accent, can))
                startConversion(app);
        } else {
            // animated progress bar
            ImVec2 a = cbP, b = ImVec2(cbP.x + cbSz.x, cbP.y + cbSz.y);
            dl->AddRectFilled(a, b, col32(withAlpha(T.panel, 0.7f)), 14.f * ui);
            float w = (b.x - a.x) * app.progressShown;
            ImVec2 fb = ImVec2(a.x + w, b.y);
            dl->AddRectFilledMultiColor(a, fb, col32(T.accent), col32(T.accent2),
                                        col32(T.accent2), col32(T.accent));
            // shimmer
            float sh = fmodf(now * 0.6f, 1.f);
            float sx = a.x + (fb.x - a.x) * sh;
            dl->AddRectFilledMultiColor(ImVec2(sx - 40 * ui, a.y), ImVec2(sx + 40 * ui, b.y),
                col32(withAlpha(ImVec4(1,1,1,1), 0.0f)), col32(withAlpha(ImVec4(1,1,1,1), 0.18f)),
                col32(withAlpha(ImVec4(1,1,1,1), 0.18f)), col32(withAlpha(ImVec4(1,1,1,1), 0.0f)));
            dl->AddRect(a, b, col32(withAlpha(T.accent, 0.5f)), 14.f * ui, 0, 1.5f);
            char ptxt[64];
            std::snprintf(ptxt, sizeof(ptxt), "Converting...  %d / %d", (int)done, (int)total);
            ImVec2 pts = ImGui::CalcTextSize(ptxt);
            dl->AddText(ImVec2((a.x + b.x) * 0.5f - pts.x * 0.5f, (a.y + b.y) * 0.5f - pts.y * 0.5f),
                        col32(ImVec4(1,1,1,1)), ptxt);
        }

        // "Open output" appears after a finished run
        if (app.justFinished && !app.converting) {
            ImGui::SetCursorScreenPos(ImVec2(cbP.x, cbP.y - 44.f * ui));
            if (fancyButton(app, "Open output folder", ImVec2(200.f * ui, 34.f * ui), T.good, true, false))
                openFolder(app.outputDir);
        }

        ImGui::End();

        // --- confetti overlay ---
        if (app.justFinished && app.finishFlash > 0.85f && app.confetti.empty() &&
            app.progress.failed.load() == 0) {
            spawnConfetti(app, ImVec2(size.x * 0.5f, size.y * 0.55f), 160);
        }
        if (!app.confetti.empty()) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            for (auto& p : app.confetti) {
                p.vel.y += 680.f * dt;
                p.vel.x *= (1.f - 0.6f * dt);
                p.pos.x += p.vel.x * dt;
                p.pos.y += p.vel.y * dt;
                p.rot += p.spin * dt;
                p.life -= dt;
                float a = clampf(p.life / p.maxLife, 0.f, 1.f);
                ImVec2 r(cosf(p.rot) * p.size, sinf(p.rot) * p.size);
                ImVec2 s(-r.y, r.x);
                ImVec2 q0(p.pos.x - r.x - s.x, p.pos.y - r.y - s.y);
                ImVec2 q1(p.pos.x + r.x - s.x, p.pos.y + r.y - s.y);
                ImVec2 q2(p.pos.x + r.x + s.x, p.pos.y + r.y + s.y);
                ImVec2 q3(p.pos.x - r.x + s.x, p.pos.y - r.y + s.y);
                fg->AddQuadFilled(q0, q1, q2, q3, col32(withAlpha(p.color, a)));
            }
            app.confetti.erase(std::remove_if(app.confetti.begin(), app.confetti.end(),
                               [&](const Particle& p) { return p.life <= 0.f || p.pos.y > size.y + 40; }),
                               app.confetti.end());
        }

        // --- toasts (bottom-center, slide + fade) ---
        {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            float ty = size.y - 92.f * ui;
            for (int i = (int)app.toasts.size() - 1; i >= 0; i--) {
                Toast& t = app.toasts[i];
                t.life -= dt;
                float a = clampf(t.life, 0.f, 1.f) * clampf((4.0f - t.life) / 0.3f, 0.f, 1.f);
                ImVec2 tsz = ImGui::CalcTextSize(t.text.c_str());
                float pdx = 16.f * ui, pdy = 9.f * ui;
                ImVec2 ta(size.x * 0.5f - tsz.x * 0.5f - pdx, ty - pdy);
                ImVec2 tb(size.x * 0.5f + tsz.x * 0.5f + pdx, ty + tsz.y + pdy);
                fg->AddRectFilled(ta, tb, col32(withAlpha(T.bg1, 0.95f * a)), 10.f * ui);
                fg->AddRect(ta, tb, col32(withAlpha(t.color, 0.8f * a)), 10.f * ui, 0, 1.5f);
                fg->AddText(ImVec2(size.x * 0.5f - tsz.x * 0.5f, ty), col32(withAlpha(t.color, a)),
                            t.text.c_str());
                ty -= (tb.y - ta.y) + 8.f * ui;
            }
            while (!app.toasts.empty() && app.toasts.front().life <= 0.f) app.toasts.pop_front();
        }

        ImGui::Render();
        int fbw, fbh;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (app.worker.joinable()) {
        app.progress.cancel.store(true);
        app.worker.join();
    }
    NFD_Quit();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
