#include "mod/overlay.h"

#include "core/match_code.h"
#include "mod/config.h"
#include "mod/files.h"
#include "mod/flow.h"
#include "mod/hitch.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/netplay_driver.h"
#include "mod/status.h"
#include "mod/updater.h"
#include "mod/weyve_service.h"

#include <MinHook.h>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_internal.h> // key ownership (Stepper)
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <xinput.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace sfv::mod {

namespace {

// IDXGISwapChain vtable slots.
constexpr int kPresentSlot = 8;
constexpr int kResizeBuffersSlot = 13;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* self, UINT syncInterval, UINT flags);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* self, UINT count, UINT width, UINT height,
                                                    DXGI_FORMAT format, UINT flags);
PresentFn g_origPresent = nullptr;
ResizeBuffersFn g_origResizeBuffers = nullptr;
void* g_presentTarget = nullptr;
void* g_resizeTarget = nullptr;

// Render thread state (Present and ResizeBuffers run there).
IDXGISwapChain* g_swapChain = nullptr; // the one ImGui was set up for
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_window = nullptr;
bool g_imgui = false;

std::atomic<bool> g_visible{false};
std::atomic<bool> g_voting{false};        // the post-match vote is up and takes input
std::atomic<HWND> g_inputWindow{nullptr}; // the window ImGui reads input for
std::string g_build;                      // the mod's version (SetOverlayBuild)
std::atomic<float> g_fps{0};              // presented frames per second (RenderFps)
bool g_inMatch = false;                   // render thread: a netplay match runs (for the frame-drop log)
std::string g_packResult;                 // render thread: what the last "Pack logs to share" did
std::atomic<bool> g_noVsync{false};       // present without waiting for the display's refresh
std::atomic<UINT> g_gameSyncInterval{0};  // the sync interval and flags the game presents with (VsyncInfo)
std::atomic<UINT> g_gamePresentFlags{0};
std::atomic<int64_t> g_presentMicros{0};  // ... time in Present, summed with the frames (VsyncInfo)
std::atomic<int64_t> g_presentFrames{0};

// Window messages for ImGui, from the window thread to the render thread.
struct Message {
    UINT msg;
    WPARAM wp;
    LPARAM lp;
};
std::mutex g_inputMutex;
std::vector<Message> g_input;

uint16_t g_lastButtons[XUSER_MAX_COUNT] = {};
std::wstring g_panelPath;
std::atomic<bool> g_focusPanel{false}; // just opened: give it the keyboard/pad focus

// The pads' latest real state, for ImGui's gamepad navigation.
struct PadState {
    std::atomic<uint16_t> buttons{0};
    std::atomic<int16_t> lx{0}, ly{0};
};
PadState g_pads[XUSER_MAX_COUNT];

// All pads together, as ImGui gamepad keys.
void FeedPads(ImGuiIO& io)
{
    uint16_t buttons = 0;
    int lx = 0, ly = 0;
    for (const PadState& p : g_pads) {
        buttons |= p.buttons.load();
        if (std::abs(p.lx.load()) > std::abs(lx)) lx = p.lx;
        if (std::abs(p.ly.load()) > std::abs(ly)) ly = p.ly;
    }
    struct Map {
        uint16_t button;
        ImGuiKey key;
    };
    static const Map kButtons[] = {
        {XINPUT_GAMEPAD_A, ImGuiKey_GamepadFaceDown},   {XINPUT_GAMEPAD_B, ImGuiKey_GamepadFaceRight},
        {XINPUT_GAMEPAD_X, ImGuiKey_GamepadFaceLeft},   {XINPUT_GAMEPAD_Y, ImGuiKey_GamepadFaceUp},
        {XINPUT_GAMEPAD_DPAD_UP, ImGuiKey_GamepadDpadUp}, {XINPUT_GAMEPAD_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
        {XINPUT_GAMEPAD_DPAD_LEFT, ImGuiKey_GamepadDpadLeft}, {XINPUT_GAMEPAD_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, ImGuiKey_GamepadL1}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, ImGuiKey_GamepadR1},
        {XINPUT_GAMEPAD_START, ImGuiKey_GamepadStart},  {XINPUT_GAMEPAD_BACK, ImGuiKey_GamepadBack},
    };
    for (const Map& m : kButtons) io.AddKeyEvent(m.key, (buttons & m.button) != 0);
    constexpr float kDead = 7849.0f; // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
    auto axis = [&](ImGuiKey key, float v) {
        const float a = std::clamp((v - kDead) / (32767.0f - kDead), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, a > 0.1f, a);
    };
    axis(ImGuiKey_GamepadLStickLeft, -static_cast<float>(lx));
    axis(ImGuiKey_GamepadLStickRight, static_cast<float>(lx));
    axis(ImGuiKey_GamepadLStickUp, static_cast<float>(ly));
    axis(ImGuiKey_GamepadLStickDown, -static_cast<float>(ly));
}

void ReleaseTarget()
{
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool CreateTarget(IDXGISwapChain* swapChain)
{
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
    const HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    return SUCCEEDED(hr);
}

void ReleaseImGui()
{
    if (g_imgui) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui = false;
    }
    ReleaseTarget();
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
    g_swapChain = nullptr;
    g_inputWindow = nullptr;
}

// ----- the look: the game's menus ------------------------------------------
// From SFV's main menu: near-black bars, each entry a light warm gray tile
// with dark gray text and a cream outline, the focused one dark olive with
// gold text over a gold rule.
const ImVec4 kGold(1.00f, 0.89f, 0.55f, 1.0f);     // #FFE38C, focused text and titles
const ImVec4 kGoldLine(0.85f, 0.72f, 0.36f, 1.0f); // #D8B85B, rules
const ImVec4 kCream(0.99f, 0.98f, 0.91f, 1.0f);    // #FCFBE7, tile outlines
const ImVec4 kTile(0.82f, 0.82f, 0.78f, 1.0f);     // #D0D1C7
const ImVec4 kTileText(0.27f, 0.27f, 0.27f, 1.0f); // #454545
const ImVec4 kOlive(0.22f, 0.22f, 0.17f, 1.0f);    // #39392B, the focused tile
const ImVec4 kP1(0.36f, 0.62f, 1.0f, 1.0f);
const ImVec4 kP2(1.0f, 0.36f, 0.30f, 1.0f);
const ImVec4 kRed(1.0f, 0.36f, 0.30f, 1.0f);
const ImVec4 kGreen(0.55f, 0.86f, 0.45f, 1.0f);
const ImVec4 kDim(0.66f, 0.65f, 0.59f, 1.0f);

float g_uiScale = 1.0f;          // sizes: 1 at 720 lines (render thread)
float g_builtScale = 0.0f;       // what the fonts were made for
ImFont* g_fontHeading = nullptr; // bold italic, as the menus' entries
ImFont* g_fontBig = nullptr;     // match codes
ImFont* g_fontSmall = nullptr;   // the player bars' details
ImFont* g_fontTiny = nullptr;    // ... their frame rate and rollback numbers

float UiScaleFor(float height) { return std::max(1.0f, height / 720.0f); }

ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

void ApplyTheme(float scale)
{
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    // Square corners and thin outlines, as the game's tiles.
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(7, 5);
    s.WindowBorderSize = s.FrameBorderSize = s.PopupBorderSize = 1;
    s.TabBorderSize = 0;
    s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = 0;
    s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0;
    s.ScrollbarSize = 12;
    s.GrabMinSize = 12;
    s.ScaleAllSizes(scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = Rgb(242, 240, 230);
    c[ImGuiCol_TextDisabled] = Rgb(140, 138, 126);
    c[ImGuiCol_WindowBg] = Rgb(20, 20, 18, 0.97f);
    c[ImGuiCol_ChildBg] = Rgb(0, 0, 0, 0.25f);
    c[ImGuiCol_PopupBg] = Rgb(26, 26, 22, 0.98f);
    c[ImGuiCol_Border] = Rgb(216, 212, 178, 0.45f);
    c[ImGuiCol_BorderShadow] = Rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = Rgb(42, 42, 34);
    c[ImGuiCol_FrameBgHovered] = Rgb(57, 57, 43);
    c[ImGuiCol_FrameBgActive] = Rgb(72, 71, 55);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = Rgb(10, 10, 10);
    c[ImGuiCol_MenuBarBg] = Rgb(10, 10, 10);
    c[ImGuiCol_ScrollbarBg] = Rgb(10, 10, 10, 0.5f);
    c[ImGuiCol_ScrollbarGrab] = Rgb(90, 89, 72);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(120, 118, 95);
    c[ImGuiCol_ScrollbarGrabActive] = kGoldLine;
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_SliderGrab] = kGoldLine;
    c[ImGuiCol_SliderGrabActive] = kGold;
    c[ImGuiCol_Button] = Rgb(46, 46, 37);
    c[ImGuiCol_ButtonHovered] = Rgb(57, 57, 43);
    c[ImGuiCol_ButtonActive] = Rgb(72, 71, 55);
    c[ImGuiCol_Header] = Rgb(57, 57, 43);
    c[ImGuiCol_HeaderHovered] = Rgb(72, 71, 55);
    c[ImGuiCol_HeaderActive] = Rgb(88, 86, 66);
    c[ImGuiCol_Separator] = Rgb(216, 184, 91, 0.45f);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = kGoldLine;
    c[ImGuiCol_ResizeGrip] = Rgb(216, 184, 91, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = Rgb(216, 184, 91, 0.6f);
    c[ImGuiCol_ResizeGripActive] = kGoldLine;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = Rgb(30, 30, 25);
    c[ImGuiCol_TabHovered] = Rgb(72, 71, 55);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = kOlive;
    c[ImGuiCol_TabSelectedOverline] = kGoldLine;
    c[ImGuiCol_TabDimmedSelectedOverline] = Rgb(216, 184, 91, 0.5f);
    c[ImGuiCol_TableHeaderBg] = Rgb(30, 30, 25);
    c[ImGuiCol_TableBorderStrong] = Rgb(216, 212, 178, 0.35f);
    c[ImGuiCol_TableBorderLight] = Rgb(216, 212, 178, 0.18f);
    c[ImGuiCol_TableRowBg] = Rgb(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = Rgb(255, 255, 255, 0.03f);
    c[ImGuiCol_TextSelectedBg] = Rgb(216, 184, 91, 0.35f);
    c[ImGuiCol_TextLink] = kGold;
    c[ImGuiCol_DragDropTarget] = kGold;
    c[ImGuiCol_NavCursor] = kGold;
    c[ImGuiCol_NavWindowingHighlight] = kGold;
    c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = Rgb(0, 0, 0, 0.5f);
}

// Fonts for the screen's size (built again when it changes): Bahnschrift
// for text, Segoe UI Bold Italic for titles and tiles, as the menus' bold
// italic entries; ImGui's own font when Windows lacks them.
void BuildFonts(float scale)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    char dir[MAX_PATH] = {};
    GetWindowsDirectoryA(dir, MAX_PATH);
    const std::string fonts = std::string(dir) + "\\Fonts\\";
    auto add = [&](const char* file, float px) -> ImFont* {
        const std::string path = fonts + file;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        return io.Fonts->AddFontFromFileTTF(path.c_str(), px * scale, &cfg);
    };
    ImFont* body = add("bahnschrift.ttf", 17);
    if (!body) {
        ImFontConfig cfg;
        cfg.SizePixels = 13 * scale;
        body = io.Fonts->AddFontDefault(&cfg);
    }
    g_fontHeading = add("segoeuiz.ttf", 20);
    if (!g_fontHeading) g_fontHeading = body;
    g_fontBig = add("bahnschrift.ttf", 40);
    if (!g_fontBig) g_fontBig = body;
    g_fontSmall = add("bahnschrift.ttf", 14);
    if (!g_fontSmall) g_fontSmall = body;
    g_fontTiny = add("bahnschrift.ttf", 12);
    if (!g_fontTiny) g_fontTiny = g_fontSmall;
    io.FontDefault = body;
    io.FontGlobalScale = 1.0f;
    ApplyTheme(scale);
    g_builtScale = g_uiScale = scale;
}

std::string Upper(std::string text)
{
    for (char& ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return text;
}

// A gold rule across the window that fades out to the right, as under the
// menus' focused entry.
void Rule()
{
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = std::max(1.0f, 1.5f * g_uiScale);
    const ImU32 solid = ImGui::GetColorU32(kGoldLine);
    const ImU32 clear = ImGui::GetColorU32(ImVec4(kGoldLine.x, kGoldLine.y, kGoldLine.z, 0.0f));
    d->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), solid, clear, clear, solid);
    ImGui::Dummy(ImVec2(0, h));
}

// A section title: bold italic capitals in gold over a rule.
void Heading(const char* text)
{
    ImGui::PushFont(g_fontHeading);
    ImGui::TextColored(kGold, "%s", Upper(text).c_str());
    ImGui::PopFont();
    Rule();
}

// A main action as the menus draw their entries: a light tile with dark
// text, dark olive with gold text over a gold rule when focused, hovered or
// chosen. Width 0 fits the label, -1 fills the row.
bool TileButton(const char* label, float width = 0, bool chosen = false)
{
    std::string text = label;
    if (const size_t hash = text.find("##"); hash != std::string::npos) text.resize(hash);
    text = Upper(text);
    ImGui::PushFont(g_fontHeading);
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const float pad = st.FramePadding.x * 1.6f;
    if (width < 0) width = ImGui::GetContentRegionAvail().x;
    const ImVec2 size(width > 0 ? width : textSize.x + pad * 2, textSize.y + st.FramePadding.y * 4.4f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    // (Invisible buttons are left out of pad and keyboard navigation unless asked.)
    const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    const bool lit = chosen || ImGui::IsItemHovered() || ImGui::IsItemFocused() || ImGui::IsItemActive();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + size.x, at.y + size.y);
    d->AddRectFilled(at, end, ImGui::GetColorU32(lit ? kOlive : kTile));
    d->AddRect(at, end, ImGui::GetColorU32(lit ? kGoldLine : kCream), 0, 0, std::max(1.0f, g_uiScale));
    const float textY = at.y + (size.y - textSize.y) * 0.5f;
    if (lit) {
        const float y = textY + textSize.y + st.FramePadding.y * 0.4f;
        const ImU32 solid = ImGui::GetColorU32(kGoldLine);
        const ImU32 clear = ImGui::GetColorU32(ImVec4(kGoldLine.x, kGoldLine.y, kGoldLine.z, 0.0f));
        d->AddRectFilledMultiColor(ImVec2(at.x + pad, y), ImVec2(end.x - pad, y + std::max(1.0f, g_uiScale)), solid, clear, clear, solid);
    }
    d->AddText(ImVec2(at.x + pad, textY), ImGui::GetColorU32(lit ? kGold : kTileText), text.c_str());
    ImGui::PopFont();
    return pressed;
}

// Two tiles side by side, splitting the row.
float HalfRow() { return (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f; }

// Latency at a glance: green under 80 ms, yellow to 149, red from 150.
ImVec4 PingColor(unsigned ms)
{
    if (ms < 80) return kGreen;
    if (ms < 150) return Rgb(255, 210, 80);
    return kRed;
}

// Peer to peer, or relayed (through whose TURN: Cloudflare's, for the
// public server). Short for the player bars, long for the lobby.
std::string PathText(const NetView& v, bool brief)
{
    switch (v.linkPath) {
    case NetView::Path::Direct: return brief ? "P2P" : "Peer to peer";
    case NetView::Path::Relayed: {
        if (brief) return "RELAY";
        std::string host = v.relayHost;
        if (host.find("cloudflare") != std::string::npos) host = "Cloudflare";
        return host.empty() ? "Relayed (TURN)" : "Relayed via " + host;
    }
    case NetView::Path::Unknown: break;
    }
    return "";
}

// Relayed links add the relay's detour: shown in yellow, a direct one dim.
ImVec4 PathColor(const NetView& v) { return v.linkPath == NetView::Path::Relayed ? Rgb(255, 210, 80) : kDim; }

const char* LinkText(lobby::Link link)
{
    switch (link) {
    case lobby::Link::Wired: return "Wired";
    case lobby::Link::WiFi: return "Wi-Fi";
    case lobby::Link::Unknown: break;
    }
    return "";
}

// A connection's icon in a square of `size` from `at`: Wi-Fi's arcs over a
// dot, or a wired network plug; nothing when unknown.
void DrawLinkIcon(ImDrawList* d, ImVec2 at, float size, lobby::Link link, ImU32 color)
{
    const float line = std::max(1.0f, size * 0.11f);
    if (link == lobby::Link::WiFi) {
        const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.86f);
        constexpr float kPi = 3.14159265f;
        for (float r : {0.3f, 0.55f, 0.8f}) {
            d->PathArcTo(c, size * r, -kPi * 0.75f, -kPi * 0.25f, 12);
            d->PathStroke(color, ImDrawFlags_None, line);
        }
        d->AddCircleFilled(c, size * 0.09f, color);
    }
    else if (link == lobby::Link::Wired) {
        // A plug's head with its contacts, and the cable below it.
        const ImVec2 a(at.x + size * 0.2f, at.y + size * 0.12f), b(at.x + size * 0.8f, at.y + size * 0.62f);
        d->AddRect(a, b, color, 0, 0, line);
        for (float x : {0.38f, 0.5f, 0.62f}) {
            d->AddLine(ImVec2(at.x + size * x, a.y + size * 0.1f), ImVec2(at.x + size * x, a.y + size * 0.26f), color, line * 0.8f);
        }
        d->AddRectFilled(ImVec2(at.x + size * 0.36f, b.y), ImVec2(at.x + size * 0.64f, b.y + size * 0.12f), color);
        d->AddLine(ImVec2(at.x + size * 0.5f, b.y + size * 0.12f), ImVec2(at.x + size * 0.5f, at.y + size * 0.98f), color, line);
    }
}

// A relayed link's icon in a square of `size` from `at`: a cloud, the relay
// (Cloudflare's) the datagrams pass through.
void DrawRelayIcon(ImDrawList* d, ImVec2 at, float size, ImU32 color)
{
    auto p = [&](float x, float y) { return ImVec2(at.x + size * x, at.y + size * y); };
    d->AddRectFilled(p(0.12f, 0.56f), p(0.88f, 0.84f), color, size * 0.14f);
    d->AddCircleFilled(p(0.33f, 0.58f), size * 0.2f, color);
    d->AddCircleFilled(p(0.56f, 0.46f), size * 0.27f, color);
    d->AddCircleFilled(p(0.76f, 0.62f), size * 0.19f, color);
}

// The icon inline in a window, as a text-sized item.
void LinkIcon(lobby::Link link, ImVec4 color)
{
    const float size = ImGui::GetTextLineHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    DrawLinkIcon(ImGui::GetWindowDrawList(), ImVec2(at.x, at.y + (ImGui::GetFrameHeight() - size) * 0.5f), size, link,
                 ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(size, ImGui::GetFrameHeight()));
}

// "I", "II", "III": how the menus number V-Skills and V-Triggers.
std::string Roman(int n)
{
    static const char* kRoman[] = {"I", "II", "III", "IV", "V"};
    return n >= 1 && n <= 5 ? kRoman[n - 1] : std::to_string(n);
}

// The other player's frame rate averaged over the last 3 s: it moves
// around 60 as rollback catches up or the game slows to stay in sync.
int SmoothedFps(int seat, int fps)
{
    struct Sample {
        ULONGLONG at;
        int fps;
    };
    static std::vector<Sample> samples[kNumPlayers];
    auto& s = samples[seat];
    const ULONGLONG now = GetTickCount64();
    if (fps > 0) s.push_back({now, fps});
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [now](const Sample& x) { return now - x.at <= 3000; }));
    if (s.empty()) return fps;
    double sum = 0;
    for (const Sample& x : s) sum += x.fps;
    return static_cast<int>(sum / s.size() + 0.5);
}

// First Present of a swap chain: ImGui for its device and window.
bool SetUp(IDXGISwapChain* swapChain)
{
    ReleaseImGui();
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return false;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) {
        Log("overlay: the swap chain has no D3D11 device");
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    g_window = desc.OutputWindow;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing written next to the game
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplWin32_Init(g_window);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad; // fed by FeedPads
    ImGui_ImplDX11_Init(g_device, g_context);
    BuildFonts(UiScaleFor(static_cast<float>(desc.BufferDesc.Height)));
    g_imgui = true;
    g_swapChain = swapChain;
    g_inputWindow = g_window;
    Log("overlay: set up on swap chain %p (window %p, %ux%u)", static_cast<void*>(swapChain), static_cast<void*>(g_window),
        desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

// Drawing the Debug tab's previews of the match windows (render thread):
// their buttons and choices then act on nothing.
bool g_previewing = false;

// Runs a netplay action off the render thread (hosting and joining open
// sockets, leaving waits for threads).
void Act(std::function<void()> action)
{
    if (g_previewing) return;
    std::thread([action = std::move(action)] {
        ThreadGuard running;
        action();
    }).detach();
}

// The game's character and stage tables, read once they exist (validating
// memory is too slow to repeat every frame).
struct Tables {
    std::vector<std::string> charas; // playable characters
    std::vector<int> charaIndex;     // each one's index in the game's character table
    std::vector<std::string> labels; // "CHUN-LI"
    // Per character, the costumes the game's menus offer (indices into its
    // costume list, in its order), each one's name and its color ids (the
    // game's CharaSpec table: 0.. its colors, 100.. its EX colors).
    std::vector<std::vector<int>> costumes;
    std::vector<std::vector<std::string>> costumeNames;
    std::vector<std::vector<std::vector<int>>> colors;
    std::vector<std::string> stages; // the stage select's stages
    std::vector<std::string> stageLabels;           // "Bustling Side Street"
    std::vector<std::vector<std::string>> settings; // per stage: its settings' stage codes (The Grid: S25)
    std::vector<std::vector<std::string>> settingLabels; // their names ("The Grid Alternative")
    std::vector<std::vector<std::string>> vskills;   // per character, the game's names
    std::vector<std::vector<std::string>> vtriggers;
    bool loaded = false;
};
Tables g_tables;

// Reads the tables on a worker thread: the names walk every object (seconds),
// which on the render thread froze the game.
Tables ReadTables()
{
    Tables t;
    for (int i = 0; i < CharaCount() && !Stopping(); i++) {
        // Entries with a handful of costumes or fewer are not fighters
        // (random select, dummies, assist and event characters).
        if (CharaCostumeCount(i) < 10) continue;
        // Placeholders: "*NDK" and untranslated "ID_..." names; X codes are
        // helpers (X01: the partner Akira's V-Trigger calls in).
        const std::string name = CharaName(i);
        if (name.empty() || name[0] == '*' || name.rfind("ID_", 0) == 0 || CharaCode(i).rfind('X', 0) == 0) continue;
        std::vector<int> costumes;
        std::vector<std::string> names;
        std::vector<std::vector<int>> colors;
        const std::map<int, CostumeSpec> specs = CharaCostumeSpecs(i);
        for (int k = 0; k < CharaCostumeCount(i); k++) {
            const int n = CostumeColorCount(i, k);
            if (n <= 0) continue;
            costumes.push_back(k);
            const auto spec = specs.find(k);
            const bool named = spec != specs.end() && !spec->second.name.empty() && spec->second.name.rfind("ID_", 0) != 0;
            names.push_back(named ? spec->second.name : "Costume " + std::to_string(costumes.size()));
            std::vector<int> ids = spec != specs.end() ? spec->second.colors : std::vector<int>{};
            if (ids.empty()) {
                for (int c = 0; c < n; c++) ids.push_back(c);
            }
            colors.push_back(std::move(ids));
        }
        if (costumes.empty()) continue;
        t.charas.push_back(CharaCode(i));
        t.charaIndex.push_back(i);
        t.labels.push_back(name);
        t.costumes.push_back(std::move(costumes));
        t.costumeNames.push_back(std::move(names));
        t.colors.push_back(std::move(colors));
        t.vskills.push_back(SkillNames(CharaCode(i), false));
        t.vtriggers.push_back(SkillNames(CharaCode(i), true));
    }
    for (int i = 0; i < StageCount() && !Stopping(); i++) {
        if (!IsMenuStage(i)) continue;
        t.stages.push_back(StageCode(i));
        const std::string name = StageName(i);
        t.stageLabels.push_back(name.empty() ? StageCode(i) : name);
        std::vector<std::string> codes, names;
        for (int k : StageSettings(i)) {
            if (!IsPlayableStage(k)) continue;
            codes.push_back(StageCode(k));
            names.push_back(StageName(k).empty() ? StageCode(k) : StageName(k));
        }
        t.settings.push_back(std::move(codes));
        t.settingLabels.push_back(std::move(names));
    }
    t.loaded = !t.charas.empty() && !t.stages.empty();
    return t;
}

std::mutex g_tablesMutex;
std::unique_ptr<Tables> g_readTables; // from the worker, taken by the render thread
std::atomic<bool> g_readingTables{false};

// Render thread: starts the read, and takes its result once there.
void LoadTables()
{
    if (g_tables.loaded) return;
    {
        std::lock_guard lock(g_tablesMutex);
        if (g_readTables) {
            if (g_readTables->loaded) g_tables = std::move(*g_readTables);
            g_readTables.reset();
            g_readingTables = false; // not loaded yet (game still starting): read again later
            return;
        }
    }
    if (g_readingTables.exchange(true)) return;
    Act([] {
        auto t = std::make_unique<Tables>(ReadTables());
        if (!t->loaded) Sleep(1000); // the game has not loaded them yet
        std::lock_guard lock(g_tablesMutex);
        g_readTables = std::move(t);
    });
}

// Where a costume (the game's index) is in the character's offered list; -1
// when it is not offered.
int CostumeSlot(int chara, int costume)
{
    const std::vector<int>& costumes = g_tables.costumes[chara];
    const auto it = std::find(costumes.begin(), costumes.end(), costume);
    return it == costumes.end() ? -1 : static_cast<int>(it - costumes.begin());
}

// What the player chose in the panel; applied to the netplay options when
// hosting or joining (and kept there for F7/F8).
struct Choices {
    bool initialised = false;
    bool pick = false;
    int chara = 0;
    int costume = 0;
    int color = 0;    // the game's color id: 0.. the costume's colors, 100.. its EX colors
    int vskill = 1;   // from 1
    int vtrigger = 1; // from 1
    int variant = 0;  // the costume's variant (dress state), 0: none
    int stage = 1;    // 1 random, 2.. a stage
    int setting = 0;  // 0 the stage's default, n its n-th setting
    int delay = 2;
    char code[64] = "";
    char name[lobby::kMaxName + 1] = "";
    bool allowSpectators = true;
    bool showFps = false;
};
Choices g_choices;

void InitChoices()
{
    if (g_choices.initialised || !g_tables.loaded) return;
    const NetplayOptions o = NetOptions();
    // A session always plays a pick (there is no character select to fall
    // back on): the saved one, else the first character.
    g_choices.pick = true;
    if (o.hasPick) {
        const std::string code = UnpackCode(o.pick.character);
        const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
        if (it != g_tables.charas.end()) g_choices.chara = static_cast<int>(it - g_tables.charas.begin());
        g_choices.costume = o.pick.costume;
        g_choices.color = o.pick.color;
        g_choices.vskill = o.pick.vskill + 1;
        g_choices.vtrigger = o.pick.vtrigger + 1;
        g_choices.variant = o.pick.variation;
    }
    // A saved stage is a stage select stage or one of its settings.
    g_choices.stage = 1;
    for (int i = 0; i < static_cast<int>(g_tables.stages.size()) && !o.stageCode.empty(); i++) {
        if (g_tables.stages[i] == o.stageCode) g_choices.stage = 2 + i;
        const auto& settings = g_tables.settings[i];
        const auto it = std::find(settings.begin(), settings.end(), o.stageCode);
        if (it != settings.end()) {
            g_choices.stage = 2 + i;
            g_choices.setting = 1 + static_cast<int>(it - settings.begin());
        }
    }
    g_choices.delay = o.inputDelay;
    std::snprintf(g_choices.name, sizeof(g_choices.name), "%s", o.name.c_str());
    g_choices.allowSpectators = o.allowSpectators;
    g_choices.showFps = o.showOwnFps;
    g_choices.initialised = true;
}

// The netplay options with the panel's choices.
NetplayOptions ChosenOptions()
{
    NetplayOptions o = NetOptions();
    const Choices& c = g_choices;
    o.hasPick = c.pick && c.chara < static_cast<int>(g_tables.charas.size());
    if (o.hasPick) {
        o.pick.character = PackCode(g_tables.charas[c.chara].c_str());
        o.pick.costume = static_cast<uint16_t>(c.costume);
        o.pick.color = static_cast<uint8_t>(c.color);
        o.pick.vskill = static_cast<uint8_t>(c.vskill - 1);
        o.pick.vtrigger = static_cast<uint8_t>(c.vtrigger - 1);
        o.pick.variation = static_cast<uint8_t>(c.variant);
        o.pick.comment = 0;
    }
    o.stage = kKeepStage;
    o.stageCode = "random"; // the host picks one per match
    if (c.stage >= 2 && c.stage - 2 < static_cast<int>(g_tables.stages.size())) {
        const int i = c.stage - 2;
        const auto& settings = g_tables.settings[i];
        o.stageCode = c.setting > 0 && c.setting <= static_cast<int>(settings.size()) ? settings[c.setting - 1] : g_tables.stages[i];
    }
    o.inputDelay = static_cast<uint8_t>(c.delay);
    o.name = c.name;
    o.allowSpectators = c.allowSpectators;
    o.showOwnFps = c.showFps;
    return o;
}

std::string CharaLabel(const std::string& code)
{
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    return it == g_tables.charas.end() ? code : g_tables.labels[it - g_tables.charas.begin()];
}

std::string SkillText(const std::string& code, bool vtrigger, unsigned index)
{
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    if (it != g_tables.charas.end()) {
        const auto& names = (vtrigger ? g_tables.vtriggers : g_tables.vskills)[it - g_tables.charas.begin()];
        if (index < names.size() && !names[index].empty()) return Roman(static_cast<int>(index) + 1) + " " + names[index];
    }
    return Roman(static_cast<int>(index) + 1);
}

// A color as the menus name it: "Color 3", "EX Color 2" (EX colors are ids
// from 100).
std::string ColorLabel(int id)
{
    return id >= 100 ? "EX Color " + std::to_string(id - 99) : "Color " + std::to_string(id + 1);
}

// The game's name of a character's costume (the game's index), or its number.
std::string CostumeLabel(const std::string& code, int costume)
{
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    if (it != g_tables.charas.end()) {
        const int chara = static_cast<int>(it - g_tables.charas.begin());
        const auto& costumes = g_tables.costumes[chara];
        const auto at = std::find(costumes.begin(), costumes.end(), costume);
        if (at != costumes.end()) return g_tables.costumeNames[chara][at - costumes.begin()];
    }
    return "costume " + std::to_string(costume);
}

std::string PlayerText(const PlayerSetup& p)
{
    if (!p.character) return "as the game has it";
    const std::string code = UnpackCode(p.character);
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%s  %s  %s  V-Skill %s  V-Trigger %s", CharaLabel(code).c_str(),
                  CostumeLabel(code, p.costume).c_str(), ColorLabel(p.color).c_str(), SkillText(code, false, p.vskill).c_str(),
                  SkillText(code, true, p.vtrigger).c_str());
    return buf;
}

std::string StageText(uint16_t index)
{
    const std::string code = StageCode(index);
    const auto it = std::find(g_tables.stages.begin(), g_tables.stages.end(), code);
    if (code.empty()) return "as prepared";
    return it == g_tables.stages.end() ? code : g_tables.stageLabels[it - g_tables.stages.begin()];
}

// Host, join or watch with the panel's choices; they are kept for F7/F8 and saved.
void HostOrJoin(const std::string& code, bool spectate = false)
{
    const NetplayOptions o = ChosenOptions();
    Act([o, code, spectate] {
        NetOptions() = o;
        if (!g_panelPath.empty()) SaveNetplayChoices(g_panelPath, o, o.stageCode);
        if (code.empty()) NetHost(o);
        else NetJoin(code, o, spectate);
    });
}

std::string StageRuleText(uint16_t rule)
{
    if (rule == lobby::kRandomStage) return "Random";
    if (rule == lobby::kAnyStage) return "As prepared";
    return StageText(rule);
}


// A short list stepped through one at a time: left and right (d-pad, stick,
// arrow keys) while it has the focus, or a click on its arrows. Focused, it
// owns those keys, so they change the value instead of moving the focus.
// `label` as ImGui's ("##..." hides it). True when the value changed.
bool Stepper(const char* label, int* value, int min, int max, const std::function<std::string(int)>& text)
{
    *value = std::clamp(*value, min, max);
    const float width = ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const ImGuiID id = ImGui::GetItemID();
    const bool focused = ImGui::IsItemFocused();
    const bool hovered = ImGui::IsItemHovered();
    static const ImGuiKey kLess[] = {ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft};
    static const ImGuiKey kMore[] = {ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight};
    int step = 0;
    if (focused) {
        for (ImGuiKey k : kLess) {
            ImGui::SetKeyOwner(k, id);
            if (ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, id)) step = -1;
        }
        for (ImGuiKey k : kMore) {
            ImGui::SetKeyOwner(k, id);
            if (ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, id)) step = 1;
        }
    }
    if (clicked) {
        const float x = ImGui::GetIO().MousePos.x - at.x;
        step = x < width * 0.35f ? -1 : x > width * 0.65f ? 1 : 0;
    }
    const int before = *value;
    *value = std::clamp(*value + step, min, max);
    // A frame with an arrow at each end, gold when it can step that way.
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    const ImGuiStyle& st = ImGui::GetStyle();
    d->AddRectFilled(at, end, ImGui::GetColorU32(focused ? kOlive : hovered ? st.Colors[ImGuiCol_FrameBgHovered] : st.Colors[ImGuiCol_FrameBg]));
    d->AddRect(at, end, ImGui::GetColorU32(focused ? kGoldLine : st.Colors[ImGuiCol_Border]), 0, 0, st.FrameBorderSize);
    const float s = height * 0.22f, cy = at.y + height * 0.5f;
    const float lx = at.x + st.FramePadding.x + s, rx = end.x - st.FramePadding.x - s;
    auto arrowColor = [&](bool can) { return ImGui::GetColorU32(can ? (focused ? kGold : kGoldLine) : st.Colors[ImGuiCol_TextDisabled]); };
    d->AddTriangleFilled(ImVec2(lx - s, cy), ImVec2(lx + s * 0.6f, cy - s), ImVec2(lx + s * 0.6f, cy + s), arrowColor(*value > min));
    d->AddTriangleFilled(ImVec2(rx + s, cy), ImVec2(rx - s * 0.6f, cy - s), ImVec2(rx - s * 0.6f, cy + s), arrowColor(*value < max));
    // Long names (V-Skills, V-Triggers in a narrow window) shrink to fit,
    // to 70%, then clip.
    const std::string shown = text(*value);
    const float inner = rx - lx - s * 4;
    float fontSize = ImGui::GetFontSize();
    const float full = ImGui::CalcTextSize(shown.c_str()).x;
    if (full > inner && full > 0) fontSize *= std::max(0.7f, inner / full);
    const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0, shown.c_str());
    const ImVec2 textAt(std::max(lx + s * 2, at.x + (width - size.x) * 0.5f), at.y + (height - size.y) * 0.5f);
    d->PushClipRect(ImVec2(lx + s * 2, at.y), ImVec2(lx + s * 2 + inner, end.y), true);
    d->AddText(ImGui::GetFont(), fontSize, textAt, ImGui::GetColorU32(focused ? kGold : st.Colors[ImGuiCol_Text]), shown.c_str());
    d->PopClipRect();
    // The label to the right, as ImGui's own widgets.
    std::string visible = label;
    if (const size_t hash = visible.find("##"); hash != std::string::npos) visible.resize(hash);
    if (!visible.empty()) {
        ImGui::SameLine(0, st.ItemInnerSpacing.x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(visible.c_str());
    }
    return *value != before;
}

// A V-Skill or V-Trigger choice by the game's names (numbers when unknown).
void SkillStepper(const char* label, const std::vector<std::string>& names, int* value)
{
    const int count = names.empty() ? 2 : static_cast<int>(names.size());
    Stepper(label, value, 1, count, [&](int n) {
        const std::string name = n - 1 < static_cast<int>(names.size()) ? names[n - 1] : "";
        return Roman(n) + (name.empty() ? "" : " " + name);
    });
}

// Saves the panel's settings (and keeps them for the next host or join).
void SaveChoices()
{
    const NetplayOptions o = ChosenOptions();
    Act([o] {
        NetOptions().name = o.name;
        NetOptions().allowSpectators = o.allowSpectators;
        NetOptions().showOwnFps = o.showOwnFps;
        NetOptions().inputDelay = o.inputDelay;
        if (!g_panelPath.empty()) SaveNetplayChoices(g_panelPath, o, o.stageCode);
    });
}

// The costume's variants (flow.h CostumeVariants bits), asked of the game off
// the render thread; -1 while unknown.
std::mutex g_variantMutex;
std::map<std::pair<int, int>, int> g_variants;
std::map<std::pair<int, int>, ULONGLONG> g_variantAsked;

int VariantsOf(int chara, int costume)
{
    if (chara < 0 || chara >= static_cast<int>(g_tables.charaIndex.size())) return 0;
    const std::pair<int, int> key{chara, costume};
    {
        std::lock_guard lock(g_variantMutex);
        const auto it = g_variants.find(key);
        const int known = it == g_variants.end() ? -1 : it->second;
        if (known >= 0) return known;
        ULONGLONG& asked = g_variantAsked[key];
        if (GetTickCount64() - asked < 2000) return -1;
        asked = GetTickCount64();
    }
    const int gameIndex = g_tables.charaIndex[chara];
    Act([key, gameIndex] {
        const int n = CostumeVariants(gameIndex, key.second);
        std::lock_guard lock(g_variantMutex);
        g_variants[key] = n;
    });
    return -1;
}

// The pick's parts, one widget each, for the lobby's rows and the side
// windows; `label` as ImGui's ("##..." hides it). PickValid first: it keeps
// the pick to what the game offers and says whether the lists are there.
bool PickValid(Choices& c)
{
    if (!g_tables.loaded || !c.initialised) return false;
    c.chara = std::clamp(c.chara, 0, static_cast<int>(g_tables.charas.size()) - 1);
    if (CostumeSlot(c.chara, c.costume) < 0) c.costume = g_tables.costumes[c.chara][0];
    const std::vector<int>& colors = g_tables.colors[c.chara][CostumeSlot(c.chara, c.costume)];
    if (std::find(colors.begin(), colors.end(), c.color) == colors.end()) c.color = colors[0];
    return true;
}

void PickCharacter(Choices& c, const char* label)
{
    if (ImGui::BeginCombo(label, g_tables.labels[c.chara].c_str())) {
        for (int i = 0; i < static_cast<int>(g_tables.charas.size()); i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(g_tables.labels[i].c_str(), i == c.chara)) c.chara = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    PickValid(c);
}

// Costumes by the game's names, in its list's order (the menus'); the pick
// keeps the game's index.
void PickCostume(Choices& c, const char* label)
{
    const std::vector<int>& costumes = g_tables.costumes[c.chara];
    const std::vector<std::string>& names = g_tables.costumeNames[c.chara];
    if (ImGui::BeginCombo(label, names[CostumeSlot(c.chara, c.costume)].c_str())) {
        for (size_t k = 0; k < costumes.size(); k++) {
            if (ImGui::Selectable(names[k].c_str(), costumes[k] == c.costume)) c.costume = costumes[k];
        }
        ImGui::EndCombo();
    }
    PickValid(c);
}

// The costume's colors, then its EX colors.
void PickColor(Choices& c, const char* label)
{
    const std::vector<int>& colors = g_tables.colors[c.chara][CostumeSlot(c.chara, c.costume)];
    if (ImGui::BeginCombo(label, ColorLabel(c.color).c_str())) {
        for (int id : colors) {
            if (ImGui::Selectable(ColorLabel(id).c_str(), id == c.color)) c.color = id;
        }
        ImGui::EndCombo();
    }
}

// The menus' held-button variants: the costume's own, the character's
// special one (Urien's thong, Akira's helmet off), or both; a stepper over
// those the costume has. False when it has none (nothing drawn).
bool PickVariant(Choices& c, const char* label)
{
    const int variants = VariantsOf(c.chara, c.costume);
    if (variants < 0) {
        std::string visible = label;
        if (const size_t hash = visible.find("##"); hash != std::string::npos) visible.resize(hash);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kDim, "%s%s", visible.empty() ? "" : (visible + ": ").c_str(), "checking...");
        return true;
    }
    if (variants == 0) {
        c.variant = 0;
        return false;
    }
    std::vector<int> options;
    for (int n = 0; n <= variants; n++) {
        if ((n & variants) == n) options.push_back(n);
    }
    int at = static_cast<int>(std::find(options.begin(), options.end(), c.variant) - options.begin());
    if (at >= static_cast<int>(options.size())) at = 0;
    const std::string& code = g_tables.charas[c.chara];
    Stepper(label, &at, 0, static_cast<int>(options.size()) - 1,
            [&](int i) { return VariantLabel(code, static_cast<uint8_t>(options[i])); });
    c.variant = options[at];
    return true;
}

void PickVSkill(Choices& c, const char* label) { SkillStepper(label, g_tables.vskills[c.chara], &c.vskill); }
void PickVTrigger(Choices& c, const char* label) { SkillStepper(label, g_tables.vtriggers[c.chara], &c.vtrigger); }

// This player's pick as a column of labelled widgets (the side windows).
void DrawPick(Choices& c)
{
    if (!PickValid(c)) {
        ImGui::TextDisabled("(reading the character list...)");
        return;
    }
    PickCharacter(c, "Character");
    PickCostume(c, "Costume");
    PickColor(c, "Color");
    PickVariant(c, "Variant");
    PickVSkill(c, "V-Skill");
    PickVTrigger(c, "V-Trigger");
}

// The host's stage for the next matches: random, or a stage select stage
// with one of its settings (The Grid: The Grid Alternative).
void DrawStage(Choices& c)
{
    if (!g_tables.loaded || !c.initialised) return;
    c.stage = std::clamp(c.stage, 1, static_cast<int>(g_tables.stages.size()) + 1);
    const std::string current = c.stage <= 1 ? "Random" : g_tables.stageLabels[c.stage - 2];
    if (ImGui::BeginCombo("Stage", current.c_str())) {
        if (ImGui::Selectable("Random", c.stage <= 1)) c.stage = 1;
        for (int i = 0; i < static_cast<int>(g_tables.stages.size()); i++) {
            ImGui::PushID(i);
            const bool picked = ImGui::Selectable(g_tables.stageLabels[i].c_str(), c.stage == i + 2);
            ImGui::PopID();
            if (picked) {
                c.stage = i + 2;
                c.setting = 0;
            }
        }
        ImGui::EndCombo();
    }
    if (c.stage < 2) {
        c.setting = 0;
        return;
    }
    const auto& names = g_tables.settingLabels[c.stage - 2];
    c.setting = std::clamp(c.setting, 0, static_cast<int>(names.size()));
    if (names.empty()) return;
    const std::string setting = c.setting == 0 ? "Default" : names[c.setting - 1];
    if (ImGui::BeginCombo("Setting", setting.c_str())) {
        if (ImGui::Selectable("Default", c.setting == 0)) c.setting = 0;
        for (int k = 0; k < static_cast<int>(names.size()); k++) {
            if (ImGui::Selectable(names[k].c_str(), c.setting == k + 1)) c.setting = k + 1;
        }
        ImGui::EndCombo();
    }
}

// This player's input delay for the next match, and a suggestion for it
// from the session's ping.
constexpr int kMaxDelayFrames = 10;
std::string FramesText(int n) { return std::to_string(n) + (n == 1 ? " frame" : " frames"); }

void DrawDelay(Choices& c, const char* label = "Delay")
{
    Stepper(label, &c.delay, 0, kMaxDelayFrames, FramesText);
}

// About half the round trip in frames covers the other's inputs on time;
// prediction (rollback) does the rest. 1 to 3 frames.
int SuggestedDelay(unsigned pingMs) { return std::clamp(static_cast<int>((pingMs / 2 + 8) / 17), 1, 3); }

void DrawDelaySuggestion(const NetView& v)
{
    if (!v.linkPingMs) return;
    const int suggested = SuggestedDelay(v.linkPingMs);
    ImGui::TextColored(kDim, "Suggested delay %d (ping %u ms)", suggested, v.linkPingMs);
}

void DrawDelay(Choices& c, const NetView& v)
{
    DrawDelay(c);
    DrawDelaySuggestion(v);
}

// The Settings tab: name and spectators (before a session), frame rate.
void DrawSettings(bool locked)
{
    Choices& c = g_choices;
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(260 * g_uiScale);
    bool changed = ImGui::InputTextWithHint("Name", "(Steam name)", c.name, sizeof(c.name));
    changed |= ImGui::Checkbox("Others may watch my sessions", &c.allowSpectators);
    ImGui::EndDisabled();
    if (locked) ImGui::TextColored(kDim, "Name and spectators apply from the next session.");
    ImGui::Spacing();
    if (NetOptions().udp) ImGui::TextDisabled("Transport: UDP, for testing (sfv_rollback.ini)");
    else ImGui::TextDisabled("Server: %s (change it in the Debug tab)", NetOptions().server.c_str());
    if (changed) SaveChoices();

    // The SFVR folder next to the game (files.h): what a player sends.
    ImGui::Spacing();
    Heading("Logs");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440 * g_uiScale);
    ImGui::TextColored(kDim, "Event logs and match recordings are in the SFVR folder next to the game. "
                             "To report a problem, pack them and send the zip.");
    ImGui::PopTextWrapPos();
    if (TileButton("Open folder")) OpenDataFolder();
    ImGui::SameLine();
    if (TileButton("Pack logs to share")) {
        // A few MB read and written at once: a short stall, on a click.
        std::string error;
        const std::wstring zip = PackLogsToShare(&error);
        if (zip.empty()) g_packResult = "Could not pack the logs: " + error;
        else {
            std::string name;
            for (wchar_t ch : zip.substr(zip.find_last_of(L'\\') + 1)) name += ch < 128 ? static_cast<char>(ch) : '_';
            g_packResult = "Packed " + name + " in the SFVR folder";
            OpenDataFolder(zip);
        }
    }
    if (!g_packResult.empty()) ImGui::TextColored(kDim, "%s", g_packResult.c_str());

    // Updates from the project site, installed only when asked (updater.h).
    ImGui::Spacing();
    Heading("Updates");
    const UpdateView u = UpdateViewNow();
    using U = UpdateView::State;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440 * g_uiScale);
    switch (u.state) {
    case U::Off: ImGui::TextColored(kDim, "Updates are off: %s.", u.message.c_str()); break;
    case U::Idle: ImGui::TextColored(kDim, "Not checked yet."); break;
    case U::Checking: ImGui::TextColored(kDim, "Checking for updates..."); break;
    case U::UpToDate: ImGui::TextColored(kDim, "SFVR %s is the latest.", u.current.c_str()); break;
    case U::Available:
        ImGui::TextColored(kGold, "SFVR %s is available%s%s.", u.latest.c_str(), u.published.empty() ? "" : ", from ",
                           u.published.c_str());
        if (!u.notes.empty()) ImGui::TextColored(kDim, "%s", u.notes.c_str());
        ImGui::TextColored(kDim, "Both players need the same version to play each other.");
        break;
    case U::Downloading: ImGui::TextColored(kDim, "Downloading SFVR %s...", u.latest.c_str()); break;
    case U::Ready: ImGui::TextColored(kGreen, "SFVR %s is installed. Restart the game to use it.", u.latest.c_str()); break;
    case U::Failed: ImGui::TextColored(kRed, "The update failed: %s.", u.message.c_str()); break;
    }
    ImGui::PopTextWrapPos();
    if (u.state == U::Downloading) {
        const float part = u.total ? static_cast<float>(double(u.done) / double(u.total)) : 0.0f;
        ImGui::ProgressBar(part, ImVec2(260 * g_uiScale, 0));
    }
    if (u.state == U::Available && TileButton("Download and install")) UpdateInstall();
    if (u.state == U::Available) ImGui::SameLine();
    if ((u.state == U::Idle || u.state == U::UpToDate || u.state == U::Failed || u.state == U::Available) &&
        TileButton("Check again")) {
        UpdateCheck();
    }
}

// In a session: the panel's pick, the host's stage and this player's delay
// go to the session as they change, and are saved for the next session.
void SyncChoices(const NetView& v)
{
    // Not before the panel knows the characters: it would send "no pick".
    // Not from a preview either: what it marks as sent would not be.
    if (!g_tables.loaded || !g_choices.initialised || g_previewing) return;
    static bool sentHas = false;
    static PlayerSetup sent;
    static std::string sentStage;
    static int sentDelay = -1;
    NetplayOptions o = ChosenOptions();
    // A variant goes only with a costume known to have it.
    if (const int variants = VariantsOf(g_choices.chara, g_choices.costume); variants < 0 || (o.pick.variation & variants) != o.pick.variation) {
        o.pick.variation = 0;
    }
    const bool pickChanged = o.hasPick != sentHas || !(o.pick == sent);
    const bool stageChanged = v.host && o.stageCode != sentStage;
    const bool delayChanged = o.inputDelay != sentDelay;
    if (!pickChanged && !stageChanged && !delayChanged) return;
    sentHas = o.hasPick;
    sent = o.pick;
    if (v.host) sentStage = o.stageCode;
    sentDelay = o.inputDelay;
    Act([o, pickChanged, stageChanged, delayChanged] {
        if (pickChanged) NetSetPick(o.hasPick, o.pick);
        if (stageChanged) NetSetStage(o.stageCode);
        if (delayChanged) NetSetDelay(o.inputDelay);
        if (!g_panelPath.empty()) SaveNetplayChoices(g_panelPath, o, o.stageCode);
    });
}

// An on-screen keypad for match codes, for players on a pad.
void DrawKeypad()
{
    static const char kChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    const float key = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 7) / 8;
    for (int i = 0; kChars[i]; i++) {
        if (i % 8) ImGui::SameLine();
        const char label[2] = {kChars[i], 0};
        if (ImGui::Button(label, ImVec2(key, 0))) {
            const size_t n = std::strlen(g_choices.code);
            if (n + 1 < sizeof(g_choices.code)) {
                g_choices.code[n] = kChars[i];
                g_choices.code[n + 1] = 0;
            }
        }
    }
    if (ImGui::Button("Delete", ImVec2(HalfRow(), 0))) {
        const size_t n = std::strlen(g_choices.code);
        if (n) g_choices.code[n - 1] = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(-1, 0))) g_choices.code[0] = 0;
}

const char* ChoiceText(lobby::Choice c)
{
    switch (c) {
    case lobby::Choice::Rematch: return "wants a rematch";
    case lobby::Choice::CharacterSelect: return "back to character select";
    case lobby::Choice::None: break;
    }
    return "choosing...";
}

// The character's name ("CHUN-LI"), for the short pick lines.
std::string ShortCharaName(const std::string& code) { return CharaLabel(code); }

// Both seats side by side, P1 left: name, pick in two lines and, in the
// lobby, whether ready.
void DrawSeats(const NetView& v, bool readiness)
{
    if (!ImGui::BeginTable("seats", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableNextRow();
    for (int i = 0; i < kNumPlayers; i++) {
        const lobby::Seat& s = v.seats[i];
        ImGui::TableNextColumn();
        ImGui::PushFont(g_fontHeading);
        ImGui::TextColored(i == 0 ? kP1 : kP2, "P%d", i + 1);
        ImGui::SameLine();
        if (s.present) ImGui::TextUnformatted(s.name.empty() ? "Player" : s.name.c_str());
        else ImGui::TextColored(kDim, "waiting...");
        ImGui::PopFont();
        if (s.present && v.you == i) {
            ImGui::SameLine();
            ImGui::TextColored(kDim, "(you)");
        }
        if (!s.present) continue;
        if (s.hasPick && s.pick.character) {
            const std::string code = UnpackCode(s.pick.character);
            ImGui::TextUnformatted(ShortCharaName(code).c_str());
            std::string detail = CostumeLabel(code, s.pick.costume) + ", " + ColorLabel(s.pick.color);
            if (s.pick.variation) detail += ", " + VariantLabel(code, s.pick.variation);
            ImGui::TextColored(kDim, "%s", detail.c_str());
        }
        else {
            ImGui::TextColored(kDim, "choosing...");
        }
        if (readiness) {
            if (s.ready) ImGui::TextColored(kGreen, "%s", s.preloaded ? "READY" : "READY (loading)");
            else ImGui::TextColored(kDim, "not ready");
        }
    }
    ImGui::EndTable();
}

// One quiet line: matches played, spectators, ping, the host's code.
void DrawSessionLine(const NetView& v)
{
    std::string text = "Matches " + std::to_string(v.played);
    if (v.linkPingMs) text += "   ping " + std::to_string(v.linkPingMs) + " ms";
    if (v.spectators) text += "   " + std::to_string(v.spectators) + " watching";
    if (v.host && !v.code.empty()) text += "   code " + v.code;
    ImGui::TextColored(kDim, "%s", text.c_str());
}

// Character select: a row per part of the pick with P1's column and P2's
// side by side (this player's widgets, the other's choices as text), then
// the stage, Ready and Leave.
void DrawLobby(const NetView& v)
{
    Heading("Character select");
    const bool ready = v.you >= 0 && v.seats[v.you].ready;
    const bool valid = PickValid(g_choices);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float labels = ImGui::CalcTextSize("V-Trigger").x + st.CellPadding.x * 2;
    const float column = 250 * g_uiScale;
    // A gold rule between P1's column and P2's, drawn once the table's
    // height is known.
    float ruleX = 0, ruleTop = 0;
    if (ImGui::BeginTable("lobby", 3, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##labels", ImGuiTableColumnFlags_WidthFixed, labels);
        ImGui::TableSetupColumn("##p1", ImGuiTableColumnFlags_WidthFixed, column);
        ImGui::TableSetupColumn("##p2", ImGuiTableColumnFlags_WidthFixed, column);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        for (int i = 0; i < kNumPlayers; i++) {
            const lobby::Seat& s = v.seats[i];
            ImGui::TableNextColumn();
            if (i == 1) {
                ruleX = ImGui::GetCursorScreenPos().x - st.CellPadding.x;
                ruleTop = ImGui::GetCursorScreenPos().y;
            }
            ImGui::PushFont(g_fontHeading);
            ImGui::TextColored(i == 0 ? kP1 : kP2, "P%d", i + 1);
            ImGui::SameLine();
            if (s.present) ImGui::TextUnformatted(s.name.empty() ? "Player" : s.name.c_str());
            else ImGui::TextColored(kDim, "waiting...");
            ImGui::PopFont();
            if (s.present && v.you == i) {
                ImGui::SameLine();
                ImGui::TextColored(kDim, "(you)");
            }
        }
        // Under the names, each player's connection and the ping between them.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        for (int i = 0; i < kNumPlayers; i++) {
            const lobby::Seat& s = v.seats[i];
            ImGui::TableNextColumn();
            if (!s.present) continue;
            if (s.link != lobby::Link::Unknown) {
                LinkIcon(s.link, kDim);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kDim, "%s", LinkText(s.link));
                ImGui::SameLine();
            }
            ImGui::AlignTextToFramePadding();
            if (v.linkPingMs) ImGui::TextColored(PingColor(v.linkPingMs), "%u ms", v.linkPingMs);
            else ImGui::TextColored(kDim, "measuring...");
        }
        // How the two are linked: peer to peer, or through a relay.
        if (v.linkPath != NetView::Path::Unknown) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            for (int i = 0; i < kNumPlayers; i++) {
                ImGui::TableNextColumn();
                if (v.seats[i].present) ImGui::TextColored(PathColor(v), "%s", PathText(v, false).c_str());
            }
        }
        // `mine` draws this player's widget, `theirs` the other's choice.
        auto row = [&](const char* name, const std::function<void()>& mine, const std::function<std::string(const lobby::Seat&, const std::string&)>& theirs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kDim, "%s", name);
            for (int i = 0; i < kNumPlayers; i++) {
                const lobby::Seat& s = v.seats[i];
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (i == v.you && !v.spectator && valid) {
                    ImGui::BeginDisabled(ready);
                    ImGui::PushItemWidth(-FLT_MIN);
                    mine();
                    ImGui::PopItemWidth();
                    ImGui::EndDisabled();
                }
                else {
                    ImGui::AlignTextToFramePadding();
                    const bool picked = s.present && s.hasPick && s.pick.character;
                    if (!s.present) ImGui::TextUnformatted("");
                    else if (!picked && i == v.you) ImGui::TextColored(kDim, "(reading the lists...)");
                    else if (!picked) ImGui::TextColored(kDim, "choosing...");
                    else ImGui::TextUnformatted(theirs(s, UnpackCode(s.pick.character)).c_str());
                }
                ImGui::PopID();
            }
        };
        Choices& c = g_choices;
        row("Character", [&] { PickCharacter(c, "##character"); }, [](const lobby::Seat&, const std::string& code) { return ShortCharaName(code); });
        row("Costume", [&] { PickCostume(c, "##costume"); }, [](const lobby::Seat& s, const std::string& code) { return CostumeLabel(code, s.pick.costume); });
        row("Color", [&] { PickColor(c, "##color"); }, [](const lobby::Seat& s, const std::string&) { return ColorLabel(s.pick.color); });
        row("Variant", [&] {
                if (!PickVariant(c, "##variant")) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(kDim, "none");
                }
            },
            [](const lobby::Seat& s, const std::string& code) { return VariantLabel(code, s.pick.variation); });
        row("V-Skill", [&] { PickVSkill(c, "##vskill"); }, [](const lobby::Seat& s, const std::string& code) { return SkillText(code, false, s.pick.vskill); });
        row("V-Trigger", [&] { PickVTrigger(c, "##vtrigger"); }, [](const lobby::Seat& s, const std::string& code) { return SkillText(code, true, s.pick.vtrigger); });
        row("Delay", [&] { DrawDelay(c, "##delay"); }, [](const lobby::Seat& s, const std::string&) { return FramesText(s.inputDelay); });
        // Ready, for both.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        for (int i = 0; i < kNumPlayers; i++) {
            const lobby::Seat& s = v.seats[i];
            ImGui::TableNextColumn();
            if (!s.present) continue;
            if (s.ready) ImGui::TextColored(kGreen, "%s", s.preloaded ? "READY" : "READY (loading)");
            else ImGui::TextColored(kDim, "not ready");
        }
        ImGui::EndTable();
        if (ruleX > 0) {
            const float bottom = ImGui::GetItemRectMax().y;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(ruleX - g_uiScale, ruleTop), ImVec2(ruleX + g_uiScale, bottom),
                                                      ImGui::GetColorU32(ImVec4(kGoldLine.x, kGoldLine.y, kGoldLine.z, 0.8f)));
        }
    }
    ImGui::Spacing();
    if (v.spectator) {
        ImGui::TextColored(kDim, "Stage: %s", StageRuleText(v.stageRule).c_str());
        ImGui::Text("Watching. The next match shows here when it starts.");
        if (TileButton("Stop watching", -1)) Act([] { NetLeave(); });
        return;
    }
    ImGui::PushItemWidth(column + labels);
    ImGui::BeginDisabled(ready);
    if (v.host) DrawStage(g_choices);
    else ImGui::TextColored(kDim, "Stage (the host's): %s", StageRuleText(v.stageRule).c_str());
    ImGui::EndDisabled();
    ImGui::PopItemWidth();
    DrawDelaySuggestion(v);
    ImGui::Spacing();
    // Ready locks the pick in until the match starts (netplay_driver.h).
    ImGui::BeginDisabled(ready);
    if (TileButton("Ready", HalfRow(), ready)) Act([] { NetSetReady(true); });
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (TileButton("Leave", -1)) Act([] { NetLeave(); });
    if (ready) ImGui::TextColored(kDim, "Locked in. The match starts once both players are ready.");
    DrawSessionLine(v);
    SyncChoices(v);
}

// After the match's last KO: the menu over the win screen.
void DrawPostMatch(const NetView& v)
{
    static ULONGLONG shownAt = 0;
    static uint64_t shownFor = ~0ull;
    const uint64_t key = (uint64_t(v.played) << 1) | 1;
    if (shownFor != key) {
        shownFor = key;
        shownAt = GetTickCount64();
    }
    Heading("Match over");
    DrawSeats(v, false);
    ImGui::Spacing();
    if (v.spectator) {
        ImGui::Text("Waiting for the players' choice...");
        if (TileButton("Stop watching", -1)) Act([] { NetLeave(); });
        return;
    }
    const int other = v.you == 0 ? 1 : 0;
    ImGui::TextColored(kDim, "%s %s", v.seats[other].name.c_str(), ChoiceText(v.seats[other].choice));
    const lobby::Choice mine = v.you >= 0 ? v.seats[v.you].choice : lobby::Choice::None;
    // Buttons held from the match must not choose by accident.
    ImGui::BeginDisabled(GetTickCount64() - shownAt < 750);
    if (TileButton("Rematch", -1, mine == lobby::Choice::Rematch)) Act([] { NetChoose(lobby::Choice::Rematch); });
    if (TileButton("Character select", -1, mine == lobby::Choice::CharacterSelect)) {
        Act([] { NetChoose(lobby::Choice::CharacterSelect); });
    }
    if (TileButton("Leave", -1)) Act([] { NetLeave(); });
    ImGui::EndDisabled();
    ImGui::Spacing();
    DrawDelay(g_choices, v);
    SyncChoices(v);
}

void DrawSession(const NetView& v)
{
    if (v.haveSetup) {
        ImGui::TextColored(kDim, "Stage: %s", StageText(v.setup.stage).c_str());
    }
    if (v.phase != NetView::Phase::Running) return;
    if (v.syncing) {
        ImGui::Text("Connecting to the other player...");
        return;
    }
    if (ImGui::BeginTable("stats", 2, ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char* name, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kDim, "%s", name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.c_str());
        };
        char buf[64];
        if (v.spectator) row("In hand", std::to_string(v.behind) + " frames");
        row("Ping", std::to_string(v.pingMs) + " ms");
        std::snprintf(buf, sizeof(buf), "%.1f", v.framesAhead);
        row("Frames ahead", buf);
        std::snprintf(buf, sizeof(buf), "%u (longest %u frames)", v.rollbacks, v.longestRollback);
        row("Rollbacks", buf);
        std::snprintf(buf, sizeof(buf), "%d %s, prediction %d", v.inputDelay, v.inputDelay == 1 ? "frame" : "frames", v.predictionWindow);
        row("Delay", buf);
        ImGui::EndTable();
    }
    if (v.desyncFrame >= 0) ImGui::TextColored(kRed, "DESYNC at frame %d (report saved)", v.desyncFrame);
}

std::atomic<bool> g_selectMatchTab{true};

// The Debug tab's server switch: which Weyvelength server the next host,
// join or watch goes to, with the ones used before; saved with the panel's
// choices. And relay only, to test TURN.
void DrawServerControls()
{
    static char server[128] = "";
    static bool initialised = false;
    if (!initialised) {
        std::snprintf(server, sizeof(server), "%s", NetOptions().server.c_str());
        initialised = true;
    }
    const NetView v = NetViewNow();
    const bool inSession = v.phase != NetView::Phase::Idle && v.phase != NetView::Phase::Ended;
    ImGui::SeparatorText("Server");
    ImGui::BeginDisabled(inSession);
    ImGui::SetNextItemWidth(300 * g_uiScale);
    const bool enter = ImGui::InputTextWithHint("##server", "host:port", server, sizeof(server), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    auto use = [](const std::string& chosen) {
        if (chosen.empty()) return;
        Act([chosen] {
            NetplayOptions& o = NetOptions();
            o.server = chosen;
            auto& recent = o.recentServers;
            recent.erase(std::remove(recent.begin(), recent.end(), chosen), recent.end());
            recent.insert(recent.begin(), chosen);
            if (recent.size() > 6) recent.resize(6);
            if (!g_panelPath.empty()) SaveNetplayChoices(g_panelPath, o, o.stageCode);
            Log("overlay: server %s for the next sessions", chosen.c_str());
        });
    };
    if (ImGui::Button("Use") || enter) use(server);
    ImGui::SameLine();
    if (ImGui::Button("This PC")) {
        std::snprintf(server, sizeof(server), "127.0.0.1:5555");
        use(server);
    }
    const NetplayOptions o = NetOptions();
    for (size_t k = 0; k < o.recentServers.size(); k++) {
        if (o.recentServers[k] == o.server) continue;
        ImGui::PushID(static_cast<int>(k));
        if (ImGui::SmallButton(o.recentServers[k].c_str())) {
            std::snprintf(server, sizeof(server), "%s", o.recentServers[k].c_str());
            use(server);
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::EndDisabled();
    ImGui::TextColored(kDim, "Now: %s%s", o.server.c_str(), inSession ? " (in a session: change it after)" : "");
    bool relayOnly = WeyveService::RelayOnly();
    ImGui::BeginDisabled(inSession);
    if (ImGui::Checkbox("Relay only (test TURN: links take only relay candidates)", &relayOnly)) WeyveService::SetRelayOnly(relayOnly);
    ImGui::EndDisabled();
    if (inSession && v.linkPath != NetView::Path::Unknown) {
        ImGui::TextColored(PathColor(v), "This session's link: %s", PathText(v, false).c_str());
    }
}

// The Debug tab's previews: the match windows with made-up data, each on
// its own, to look at without a session (render thread).
struct Preview {
    bool hud = false;    // the player bars over the health bars
    ULONGLONG fadeAt = 0; // their fade at the match's end, played from here
    bool strip = false;  // the corner line while the panel is closed
    bool desync = false; // the red banner
    bool side[kNumPlayers] = {false, false}; // P1's and P2's windows after a match
    int sides = 0;       // 0 the rematch vote, 1 character select over the results
    int you = 0;         // whose side is this player's
    int choice[kNumPlayers] = {0, 1}; // each side's vote: none, rematch, character select
    bool ready[kNumPlayers] = {false, true};
    bool relayed = true; // the link through Cloudflare's relay (else peer to peer)
    bool Any() const { return hud || strip || desync || side[0] || side[1]; }
};
Preview g_preview;
bool g_sampleToasts = false; // add a few toasts on the next frame

void DrawPreviewControls()
{
    Preview& p = g_preview;
    ImGui::SeparatorText("Preview the match windows (made-up data)");
    ImGui::Checkbox("Player bars", &p.hud);
    ImGui::SameLine();
    ImGui::Checkbox("Corner strip", &p.strip);
    ImGui::SameLine();
    ImGui::Checkbox("Desync banner", &p.desync);
    ImGui::SameLine();
    ImGui::Checkbox("Relayed link", &p.relayed);
    ImGui::Checkbox("P1 window", &p.side[0]);
    ImGui::SameLine();
    ImGui::Checkbox("P2 window", &p.side[1]);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * g_uiScale);
    ImGui::Combo("##sides", &p.sides, "Rematch vote\0Character select\0");
    ImGui::SetNextItemWidth(120 * g_uiScale);
    ImGui::Combo("You are", &p.you, "P1\0P2\0");
    for (int i = 0; i < kNumPlayers; i++) {
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(200 * g_uiScale);
        ImGui::Combo(i == 0 ? "P1 vote" : "P2 vote", &p.choice[i], "choosing\0Rematch\0Character select\0");
        ImGui::SameLine();
        ImGui::Checkbox(i == 0 ? "P1 ready" : "P2 ready", &p.ready[i]);
        ImGui::PopID();
    }
    if (ImGui::Button("Sample toasts")) g_sampleToasts = true;
    ImGui::SameLine();
    if (ImGui::Button("Fade the player bars")) {
        p.hud = true;
        p.fadeAt = GetTickCount64();
    }
    ImGui::SameLine();
    if (ImGui::Button("All off")) {
        const Preview defaults;
        p.hud = p.strip = p.desync = p.side[0] = p.side[1] = false;
        p.sides = defaults.sides;
    }
    ImGui::TextColored(kDim, "They stay up with the panel closed; their buttons do nothing.");
}

// Testing aids: simulated network, mashing, quick scene switches, the log.
void DrawDebug()
{
    static bool initialised = false;
    static int latency = 0, jitter = 0, loss = 0;
    static bool mash = false;
    static int seed = 1;
    if (!initialised) {
        const NetplayOptions o = NetOptions();
        latency = o.conditions.latencyMs;
        jitter = o.conditions.jitterMs;
        loss = static_cast<int>(o.conditions.loss * 100 + 0.5);
        mash = o.mashSeed != 0;
        seed = o.mashSeed ? static_cast<int>(o.mashSeed) : 1;
        initialised = true;
    }
    DrawServerControls();
    DrawPreviewControls();
    ImGui::SeparatorText("Simulated network (what this side sends)");
    ImGui::SliderInt("Latency (ms)", &latency, 0, 300);
    ImGui::SliderInt("Jitter (ms)", &jitter, 0, 100);
    ImGui::SliderInt("Loss (%)", &loss, 0, 20);
    if (ImGui::Button("Apply")) {
        NetConditions c;
        c.latencyMs = latency;
        c.jitterMs = jitter;
        c.loss = loss / 100.0;
        Act([c] {
            NetOptions().conditions = c;
            NetSetConditions(c);
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Off")) {
        latency = jitter = loss = 0;
        Act([] {
            NetOptions().conditions = NetConditions{};
            NetSetConditions(NetConditions{});
        });
    }
    ImGui::SeparatorText("Testing");
    if (ImGui::Checkbox("Mash (next match)", &mash) || (mash && ImGui::InputInt("Seed", &seed))) {
        const uint64_t value = mash ? static_cast<uint64_t>(std::max(1, seed)) : 0;
        Act([value] { NetOptions().mashSeed = value; });
    }
    if (ImGui::Button("Main menu")) ReturnToMainMenu(1);
    ImGui::SameLine();
    if (ImGui::Button("Battle now")) Act([] { JumpToBattle(); });
    ImGui::SeparatorText("Log");
    ImGui::TextColored(kDim, "instance %d, %.0f fps", StatusInstance(), ImGui::GetIO().Framerate);
    ImGui::BeginChild("log", ImVec2(720 * g_uiScale, 260 * g_uiScale), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& line : RecentLog(60)) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f); // follow the end
    ImGui::EndChild();
}

// Not in a session: host, or join with a code.
void DrawStart(const NetView& v)
{
    if (!v.result.empty()) {
        const bool bad = v.result.find("could not") != std::string::npos || v.result.find("cannot") != std::string::npos ||
                         v.result.find("not a valid") != std::string::npos || v.result.find("differ") != std::string::npos;
        ImGui::TextColored(bad ? kRed : kDim, "%s", v.result.c_str());
        ImGui::Spacing();
    }
    if (TileButton("Host a match", -1)) HostOrJoin("");
    ImGui::SetItemDefaultFocus();
    ImGui::TextColored(kDim, "You get a code to send to the other player.");
    ImGui::Spacing();
    Heading("Join");
    // The code in large type; Paste beside it.
    std::string code = g_choices.code;
    code.erase(std::remove_if(code.begin(), code.end(), [](char ch) { return ch == ' ' || ch == '\r' || ch == '\n'; }), code.end());
    MatchCode decoded;
    const bool valid = DecodeMatchCode(code, &decoded);
    const float paste = ImGui::CalcTextSize("Paste").x + ImGui::GetStyle().FramePadding.x * 4;
    ImGui::PushFont(g_fontBig);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - paste - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##code", "CODE", g_choices.code, sizeof(g_choices.code), ImGuiInputTextFlags_CharsUppercase);
    const float bigHeight = ImGui::GetItemRectSize().y;
    ImGui::PopFont();
    ImGui::SameLine();
    if (ImGui::Button("Paste", ImVec2(-1, bigHeight))) {
        if (const char* clip = ImGui::GetClipboardText()) {
            std::strncpy(g_choices.code, clip, sizeof(g_choices.code) - 1);
            g_choices.code[sizeof(g_choices.code) - 1] = 0;
        }
    }
    if (g_choices.code[0] && !valid) ImGui::TextColored(kDim, "A code is 6 characters, like ABCDEF.");
    ImGui::BeginDisabled(!valid);
    if (TileButton("Join", HalfRow())) HostOrJoin(code);
    ImGui::SameLine();
    if (TileButton("Watch", -1)) HostOrJoin(code, true);
    ImGui::EndDisabled();
    ImGui::Spacing();
    DrawKeypad();
}

// Hosting: the code, large, until the other player joins.
void DrawHosting(const NetView& v)
{
    Heading("Hosting");
    ImGui::TextUnformatted("Send this code to the other player:");
    ImGui::PushFont(g_fontBig);
    ImGui::TextColored(kGold, "%s", v.code.c_str());
    ImGui::PopFont();
    ImGui::TextColored(kDim, "Copied to the clipboard.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy again")) ImGui::SetClipboardText(v.code.c_str());
    ImGui::Spacing();
    ImGui::Text("Waiting for the other player...");
    if (v.spectators) ImGui::TextColored(kDim, "%d watching", v.spectators);
    if (TileButton("Cancel", -1)) Act([] { NetLeave(); });
}

void DrawPanel()
{
    const NetView v = NetViewNow();
    LoadTables();
    InitChoices();
    ImGuiIO& io = ImGui::GetIO();
    // Always in the middle of the screen, whatever its size.
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(460 * g_uiScale, 0), ImVec2(FLT_MAX, FLT_MAX));
    if (g_focusPanel.exchange(false)) ImGui::SetNextWindowFocus();
    ImGui::Begin("###panel", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
    // The title as the menus' black bar has it, the version on the right.
    ImGui::PushFont(g_fontHeading);
    ImGui::TextColored(kGold, "NETPLAY");
    ImGui::PopFont();
    ImGui::SameLine();
    const float version = ImGui::CalcTextSize(g_build.c_str()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - version));
    ImGui::TextDisabled("%s", g_build.c_str());
    if (!ImGui::BeginTabBar("tabs")) {
        ImGui::End();
        return;
    }
    const bool inSession = v.phase != NetView::Phase::Idle && v.phase != NetView::Phase::Ended;
    if (ImGui::BeginTabItem("Match", nullptr, ImGuiTabItemFlags_SetSelected * g_selectMatchTab.exchange(false))) {
        ImGui::Spacing();
        switch (v.phase) {
        case NetView::Phase::Idle:
        case NetView::Phase::Ended:
            DrawStart(v);
            break;
        case NetView::Phase::Connecting:
        case NetView::Phase::Joining:
            Heading("Connecting");
            ImGui::TextWrapped("%s", v.status.c_str());
            if (TileButton("Cancel", -1)) Act([] { NetLeave(); });
            break;
        case NetView::Phase::Hosting:
            DrawHosting(v);
            break;
        case NetView::Phase::Lobby:
            DrawLobby(v);
            break;
        case NetView::Phase::Jumping:
        case NetView::Phase::Loading:
            Heading("Loading");
            ImGui::TextWrapped("%s", v.status.c_str());
            DrawSeats(v, false);
            DrawSession(v);
            if (TileButton("Leave", -1)) Act([] { NetLeave(); });
            break;
        case NetView::Phase::Running:
            Heading(v.spectator ? "Watching" : "In a match");
            DrawSeats(v, false);
            DrawSession(v);
            if (TileButton("Leave", -1)) Act([] { NetLeave(); });
            break;
        case NetView::Phase::PostMatch:
            DrawPostMatch(v);
            break;
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Settings")) {
        ImGui::Spacing();
        DrawSettings(inSession);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Debug")) {
        DrawDebug();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    ImGui::Spacing();
    ImGui::TextDisabled("F1 or Back + Start closes this; the game gets no input while it is open.");
    ImGui::End();
}

// While the panel is closed: one line in a corner during a session.
void DrawStrip(const NetView& v)
{
    std::string text;
    ImVec4 color = kDim;
    char buf[160];
    switch (v.phase) {
    case NetView::Phase::Hosting: text = "Hosting " + v.code + " - waiting (F1)"; break;
    case NetView::Phase::Connecting:
    case NetView::Phase::Joining:
    case NetView::Phase::Jumping:
    case NetView::Phase::Loading: text = v.status; break;
    case NetView::Phase::Lobby: text = "Lobby (F1)"; break;
    case NetView::Phase::PostMatch: text = "Match over"; break;
    case NetView::Phase::Running:
        if (v.syncing) {
            text = "connecting...";
            break;
        }
        if (v.spectator) std::snprintf(buf, sizeof(buf), "watching  frame %d  %d in hand", v.frame, v.behind);
        else std::snprintf(buf, sizeof(buf), "%s  ping %u ms  ahead %.1f  rollbacks %u (max %u)  delay %d", v.host ? "P1" : "P2",
                           v.pingMs, v.framesAhead, v.rollbacks, v.longestRollback, v.inputDelay);
        text = buf;
        if (v.desyncFrame >= 0) {
            text += "  DESYNC at " + std::to_string(v.desyncFrame);
            color = kRed;
        }
        break;
    // How the session ended is a toast (bottom center), not repeated here.
    case NetView::Phase::Ended:
    case NetView::Phase::Idle: return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10, io.DisplaySize.y - 10), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##strip", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(color, "%s", text.c_str());
    ImGui::End();
}

// ----- without the panel: HUD, banner, toasts, the vote ---------------------

struct Toast {
    std::string text;
    ImVec4 color;
    ULONGLONG until;
};
std::vector<Toast> g_toasts; // render thread

void AddToast(const std::string& text, ImVec4 color = ImVec4(1, 1, 1, 1), int seconds = 4)
{
    if (g_toasts.size() >= 5) g_toasts.erase(g_toasts.begin());
    g_toasts.push_back({text, color, GetTickCount64() + seconds * 1000ull});
}

// Notices from what changed in the session since the last frame.
// Once per version the site offers: a toast pointing at Settings.
void ToastUpdate()
{
    static std::string toasted;
    const UpdateView u = UpdateViewNow();
    if (u.state != UpdateView::State::Available || u.latest == toasted) return;
    toasted = u.latest;
    AddToast("SFVR " + u.latest + " is available: Settings, Updates", kGold, 8);
}

void ToastChanges(const NetView& now)
{
    static NetView before;
    static bool have = false;
    if (!have) {
        before = now;
        have = true;
        return;
    }
    using P = NetView::Phase;
    if (now.phase == P::Hosting && before.phase != P::Hosting && !g_visible) {
        AddToast("Hosting: code " + now.code + " (copied). F1 for the panel.", kGreen, 10);
    }
    for (int i = 0; i < kNumPlayers; i++) {
        const lobby::Seat &a = before.seats[i], &b = now.seats[i];
        if (i == now.you) continue;
        if (now.host && b.present && !a.present && !b.name.empty()) AddToast(b.name + " joined");
        if (now.phase == P::PostMatch && before.phase == P::PostMatch && b.choice != a.choice &&
            b.choice != lobby::Choice::None) {
            AddToast(b.name + (b.choice == lobby::Choice::Rematch ? " wants a rematch" : " wants character select"));
        }
    }
    if (now.spectators != before.spectators && now.phase != P::Idle && now.phase != P::Ended) {
        AddToast(now.spectators > before.spectators ? "A spectator joined (" + std::to_string(now.spectators) + " watching)"
                                                    : std::to_string(now.spectators) + " watching",
                 kDim);
    }
    if (now.phase == P::Lobby && before.phase >= P::Jumping && before.phase <= P::PostMatch) AddToast("Back to character select");
    if (now.phase == P::Ended && before.phase != P::Ended && !now.result.empty()) AddToast("Session over: " + now.result, kRed, 6);
    before = now;
}

void DrawToasts()
{
    const ULONGLONG now = GetTickCount64();
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(), [&](const Toast& t) { return t.until <= now; }),
                   g_toasts.end());
    if (g_toasts.empty()) return;
    // All notices in one place: the bottom center, above the game's gauges.
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.86f), ImGuiCond_Always, ImVec2(0.5f, 1));
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::Begin("##toasts", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    float widest = 0;
    for (const Toast& t : g_toasts) widest = std::max(widest, ImGui::CalcTextSize(t.text.c_str()).x);
    for (const Toast& t : g_toasts) {
        ImVec4 c = t.color;
        if (t.until - now < 500) c.w = (t.until - now) / 500.0f; // fade out
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + (widest - ImGui::CalcTextSize(t.text.c_str()).x) * 0.5f);
        ImGui::TextColored(c, "%s", t.text.c_str());
    }
    ImGui::End();
}

// During a match: a bar at the very top of the screen over each health bar
// (above the game's win count), P1's on the left and P2's on the right: a
// dark plate fading out toward the timer over a gold rule, as the game's own
// name plates, on one line: the player, their connection's icon and, on the
// other player's bar, the ping (colored by latency), their frame rate (over
// the last 3 s) and the longest rollback. They fade out once the match is
// over (its last round's outro).
constexpr ULONGLONG kBarsFadeMs = 800;

void DrawPlayerBars(const NetView& v)
{
    static ULONGLONG overAt = 0;
    float alpha = 1.0f;
    if (v.phase == NetView::Phase::PostMatch) {
        if (!overAt) overAt = GetTickCount64();
        alpha = 1.0f - static_cast<float>(GetTickCount64() - overAt) / kBarsFadeMs;
        if (alpha <= 0) return;
    }
    else {
        overAt = 0;
    }
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    auto color = [alpha](ImVec4 c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a * alpha)); };
    ImFont* big = g_fontHeading;
    ImFont* text = g_fontSmall; // the details: smaller than the names, light enough to read
    const ImVec4 detail = Rgb(214, 211, 200);
    const float bigSize = big->FontSize, textSize = text->FontSize;
    const float rule = std::max(1.0f, 1.5f * g_uiScale);
    const float pad = 10 * g_uiScale, gap = 8 * g_uiScale;
    // Across, from the game's HUD measured in screen fractions (P1's health
    // bar 8-46%, P2's 54-94%, the round markers by the timer).
    const float top = 0, bottom = bigSize + 6 * g_uiScale + rule;
    const unsigned ping = v.avgPingMs > 0 ? static_cast<unsigned>(v.avgPingMs + 0.5f) : v.pingMs;
    for (int i = 0; i < kNumPlayers; i++) {
        const lobby::Seat& s = v.seats[i];
        const bool left = i == 0;
        const float x0 = left ? screen.x * 0.085f : screen.x * 0.60f;
        const float x1 = left ? screen.x * 0.40f : screen.x * 0.915f;
        // Dark at the screen's edge, clear toward the middle.
        const ImU32 dark = color(Rgb(10, 10, 9), 0.82f), clear = color(Rgb(10, 10, 9), 0.0f);
        d->AddRectFilledMultiColor(ImVec2(x0, top), ImVec2(x1, bottom), left ? dark : clear, left ? clear : dark,
                                   left ? clear : dark, left ? dark : clear);
        const ImU32 gold = color(kGoldLine), goldClear = color(kGoldLine, 0.0f);
        d->AddRectFilledMultiColor(ImVec2(x0, bottom - rule), ImVec2(x1, bottom), left ? gold : goldClear, left ? goldClear : gold,
                                   left ? goldClear : gold, left ? gold : goldClear);
        // The parts in order from the screen's edge: the name (the game
        // shows P1/P2 itself), the connection's icon, then the other
        // player's numbers: a cloud when the link is relayed (nothing when
        // it is peer to peer), the ping, and in smaller type their frame
        // rate and the longest rollback.
        enum class Kind { Text, Link, Relay };
        struct Part {
            std::string text;
            ImFont* font;
            ImU32 color;
            Kind kind = Kind::Text;
        };
        std::vector<Part> parts;
        parts.push_back({(s.name.empty() ? std::string("Player") : s.name) + (v.you == i ? " (you)" : ""), big, color(Rgb(242, 240, 230))});
        if (s.link != lobby::Link::Unknown) parts.push_back({"", text, color(detail), Kind::Link});
        const bool relayed = v.linkPath == NetView::Path::Relayed;
        if (v.spectator) {
            if (i == 1) {
                if (relayed) parts.push_back({"", text, color(PathColor(v)), Kind::Relay});
                parts.push_back({"watching, " + std::to_string(v.behind) + " in hand", text, color(detail)});
            }
        }
        else if (v.you != i) {
            char line[48];
            if (v.syncing) {
                parts.push_back({"connecting...", text, color(detail)});
            }
            else {
                if (relayed) parts.push_back({"", text, color(PathColor(v)), Kind::Relay});
                std::snprintf(line, sizeof(line), "%ums \xC2\xB1%.0f", ping, v.jitterMs);
                parts.push_back({line, text, color(PingColor(ping))});
            }
            if (s.fps) parts.push_back({std::to_string(SmoothedFps(i, s.fps)) + " fps", g_fontTiny, color(detail)});
            parts.push_back({std::to_string(v.longestRollback) + " RBF", g_fontTiny, color(detail)});
        }
        d->PushClipRect(ImVec2(x0, top), ImVec2(x1, bottom), true);
        const float mid = top + (bottom - rule - top) * 0.5f;
        float x = left ? x0 + pad : x1 - pad;
        for (const Part& part : parts) {
            const float size = part.font->FontSize;
            const float w = part.kind != Kind::Text ? textSize : part.font->CalcTextSizeA(size, FLT_MAX, 0, part.text.c_str()).x;
            const float at = left ? x : x - w;
            const ImVec2 iconAt(at, mid - textSize * 0.5f);
            if (part.kind == Kind::Link) DrawLinkIcon(d, iconAt, textSize, s.link, part.color);
            else if (part.kind == Kind::Relay) DrawRelayIcon(d, iconAt, textSize, part.color);
            else d->AddText(part.font, size, ImVec2(at, mid - size * 0.5f), part.color, part.text.c_str());
            x += left ? w + gap : -(w + gap);
        }
        d->PopClipRect();
    }
}

// A desync in this match: one small line along the bottom, until the match is
// over (the recording is in SFVR\matches, named ..._desync.sfvr).
void DrawDesyncBanner(const NetView& v)
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - io.DisplaySize.y * 0.012f), ImGuiCond_Always,
                            ImVec2(0.5f, 1));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * g_uiScale, 3 * g_uiScale));
    ImGui::Begin("##desync", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(kRed, "Desync at frame %d", v.desyncFrame);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", v.desyncReport.empty() ? "(saving the recording...)" : "(recording saved)");
    ImGui::End();
    ImGui::PopStyleVar();
}

const char* VoteText(lobby::Choice c)
{
    switch (c) {
    case lobby::Choice::Rematch: return "Rematch";
    case lobby::Choice::CharacterSelect: return "Character select";
    case lobby::Choice::None: break;
    }
    return "choosing...";
}

// The other player's pick, a line per part.
void DrawPickText(const PlayerSetup& p)
{
    if (!p.character) {
        ImGui::TextColored(kDim, "(choosing)");
        return;
    }
    const std::string code = UnpackCode(p.character);
    ImGui::TextUnformatted(ShortCharaName(code).c_str());
    const std::string variant = p.variation ? "   " + VariantLabel(code, p.variation) : "";
    ImGui::TextColored(kDim, "%s", CostumeLabel(code, p.costume).c_str());
    ImGui::TextColored(kDim, "%s%s", ColorLabel(p.color).c_str(), variant.c_str());
    ImGui::TextColored(kDim, "V-Skill %s", SkillText(code, false, p.vskill).c_str());
    ImGui::TextColored(kDim, "V-Trigger %s", SkillText(code, true, p.vtrigger).c_str());
}

// After a match, over its results screen: P1's side on the left and P2's on
// the right, where the screen leaves room. First each player's choice of
// what follows (Rematch, Character select, Leave) and input delay; once both
// chose Character select (the lobby, still over the results screen) each
// player's pick, the host's stage, and Ready. The game's own results menu is
// kept from opening meanwhile (flow.h SetHoldResults).
void DrawSides(const NetView& v, const bool (&show)[kNumPlayers] = {true, true})
{
    static ULONGLONG shownAt = 0;
    static int shownFor = -1;
    const int key = v.played * 2 + (v.phase == NetView::Phase::Lobby ? 1 : 0);
    const bool appearing = shownFor != key;
    if (appearing) {
        shownFor = key;
        shownAt = GetTickCount64();
    }
    const ImGuiIO& io = ImGui::GetIO();
    const bool lobbyPhase = v.phase == NetView::Phase::Lobby;
    // Buttons held from the match must not choose by accident: presses in
    // the first 750 ms do nothing (the buttons stay enabled so that the pad
    // focus can start on the first one).
    const bool armed = GetTickCount64() - shownAt >= 750;
    // Under a quarter of the screen each, in the bottom corners (over the
    // edge of the character portraits); the lobby's taller window grows up.
    const float width = std::max(io.DisplaySize.x * 0.23f, 300 * g_uiScale);
    for (int i = 0; i < kNumPlayers; i++) {
        if (!show[i]) continue;
        const lobby::Seat& s = v.seats[i];
        const bool mine = v.you == i;
        const float margin = io.DisplaySize.y * 0.015f;
        const float x = i == 0 ? margin : io.DisplaySize.x - margin;
        ImGui::SetNextWindowPos(ImVec2(x, io.DisplaySize.y - margin), ImGuiCond_Always, ImVec2(i == 0 ? 0.0f : 1.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 0));
        ImGui::SetNextWindowBgAlpha(0.88f);
        if (appearing && mine) ImGui::SetNextWindowFocus();
        ImGui::Begin(i == 0 ? "##side1" : "##side2", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                         (mine ? 0 : ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing));
        // Fields fill the window but for their labels ("V-Trigger" the widest).
        ImGui::PushItemWidth(-(ImGui::CalcTextSize("V-Trigger").x + ImGui::GetStyle().ItemInnerSpacing.x));
        ImGui::PushFont(g_fontHeading);
        ImGui::TextColored(i == 0 ? kP1 : kP2, "P%d", i + 1);
        ImGui::SameLine();
        ImGui::Text("%s%s", s.name.c_str(), mine ? " (you)" : "");
        ImGui::PopFont();
        Rule();
        if (!lobbyPhase) {
            ImGui::TextColored(s.choice == lobby::Choice::None ? kDim : kGreen, "%s", VoteText(s.choice));
            if (mine) {
                auto button = [&](const char* label, lobby::Choice c) {
                    const bool chosen = s.choice == c && c != lobby::Choice::None;
                    return TileButton(label, -1, chosen) && armed;
                };
                if (button("Rematch", lobby::Choice::Rematch)) Act([] { NetChoose(lobby::Choice::Rematch); });
                ImGui::SetItemDefaultFocus();
                if (button("Character select", lobby::Choice::CharacterSelect)) {
                    Act([] { NetChoose(lobby::Choice::CharacterSelect); });
                }
                if (button("Leave", lobby::Choice::None)) Act([] { NetLeave(); });
                ImGui::Spacing();
                DrawDelay(g_choices, v);
                SyncChoices(v);
            }
            else {
                ImGui::TextColored(kDim, "input delay %u", s.inputDelay);
            }
        }
        else if (mine) {
            const bool ready = s.ready;
            ImGui::BeginDisabled(ready);
            if (TileButton("Ready", -1, ready) && armed) Act([] { NetSetReady(true); });
            ImGui::EndDisabled();
            ImGui::SetItemDefaultFocus();
            if (ready) ImGui::TextColored(kDim, "Locked in. The match starts once both are ready.");
            ImGui::BeginDisabled(ready);
            DrawPick(g_choices);
            if (v.host) {
                ImGui::Spacing();
                DrawStage(g_choices);
            }
            ImGui::Spacing();
            DrawDelay(g_choices, v);
            ImGui::EndDisabled();
            ImGui::Spacing();
            if (TileButton("Leave", -1) && armed) Act([] { NetLeave(); });
            SyncChoices(v);
        }
        else {
            DrawPickText(s.pick);
            if (i == 0) ImGui::TextColored(kDim, "Stage: %s", StageRuleText(v.stageRule).c_str());
            ImGui::TextColored(kDim, "input delay %u", s.inputDelay);
            ImGui::TextColored(s.ready ? kGreen : kDim, "%s%s", s.ready ? "READY" : "choosing...",
                               s.ready && !s.preloaded ? " (loading)" : "");
        }
        if (mine && lobbyPhase && !v.host) {
            ImGui::TextColored(kDim, "Stage (the host's): %s", StageRuleText(v.stageRule).c_str());
        }
        ImGui::PopItemWidth();
        ImGui::End();
    }
}

// A made-up session for the previews: this player's pick from the panel,
// the other's another character's second costume; numbers that look like a
// match on a good connection.
NetView PreviewView(NetView::Phase phase)
{
    const Preview& p = g_preview;
    NetView v;
    v.phase = phase;
    v.you = p.you;
    v.host = p.you == 0;
    v.code = "K7Q2MX";
    v.played = 3;
    v.spectators = 1;
    v.linkPingMs = 38;
    v.stageRule = lobby::kRandomStage;
    v.overResults = true;
    const NetplayOptions mine = ChosenOptions();
    for (int i = 0; i < kNumPlayers; i++) {
        lobby::Seat& s = v.seats[i];
        s.present = true;
        s.name = i == p.you && !mine.name.empty() ? mine.name : i == 0 ? "Player One" : "Player Two";
        s.hasPick = true;
        if (i == p.you && mine.hasPick) {
            s.pick = mine.pick;
        }
        else if (g_tables.charas.size() > 1) {
            const size_t other = g_choices.chara == 1 ? 0 : 1;
            const auto& costumes = g_tables.costumes[other];
            s.pick.character = PackCode(g_tables.charas[other].c_str());
            s.pick.costume = static_cast<uint16_t>(costumes.size() > 1 ? costumes[1] : costumes[0]);
            s.pick.color = 2;
        }
        s.ready = p.ready[i];
        s.preloaded = true;
        s.choice = p.choice[i] == 1 ? lobby::Choice::Rematch : p.choice[i] == 2 ? lobby::Choice::CharacterSelect : lobby::Choice::None;
        s.wins = static_cast<uint16_t>(i == 0 ? 2 : 1);
        s.inputDelay = static_cast<uint8_t>(i == p.you ? g_choices.delay : 1);
        s.fps = 60;
        s.link = i == 0 ? lobby::Link::Wired : lobby::Link::WiFi;
    }
    v.frame = 4210;
    v.pingMs = 41;
    v.avgPingMs = 40.6f;
    v.jitterMs = 3;
    v.framesAhead = 0.4f;
    v.rollbacks = 57;
    v.longestRollback = 4;
    v.inputDelay = g_choices.delay;
    v.predictionWindow = 8;
    v.linkPath = p.relayed ? NetView::Path::Relayed : NetView::Path::Direct;
    v.relayHost = "turn.cloudflare.com";
    if (p.desync) {
        v.desyncFrame = 3812;
        v.desyncReport = "desync-preview.txt";
    }
    return v;
}

std::mutex g_previewMutex;
std::string g_previewWords; // from PreviewOverlay, applied on the render thread

// On the game's menus, with the panel closed and no session: a hint at the
// top center of how to open online play.
bool OnMenuScreen()
{
    const std::string scene = CurrentScene();
    return !scene.empty() && scene != "BootState" && scene != "InitialState" && scene.find("Battle") == std::string::npos;
}

void DrawMenuHint()
{
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const char* title = "ONLINE PLAY";
    const char* keys = "F1   or   Back + Start";
    const float gap = 14 * g_uiScale, pad = 40 * g_uiScale;
    const ImVec2 titleSize = g_fontHeading->CalcTextSizeA(g_fontHeading->FontSize, FLT_MAX, 0, title);
    const ImVec2 keysSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, keys);
    const float width = titleSize.x + gap + keysSize.x + pad * 2;
    const float rule = std::max(1.0f, 1.5f * g_uiScale);
    const float height = titleSize.y + 8 * g_uiScale + rule;
    const float x0 = (screen.x - width) * 0.5f, x1 = x0 + width;
    // A solid plate as the panel's windows: near-black, a cream outline and
    // a gold rule along its bottom.
    d->AddRectFilled(ImVec2(x0, 0), ImVec2(x1, height), ImGui::GetColorU32(Rgb(20, 20, 18, 0.95f)));
    d->AddRect(ImVec2(x0, -1), ImVec2(x1, height), ImGui::GetColorU32(Rgb(216, 212, 178, 0.6f)), 0, 0, std::max(1.0f, g_uiScale));
    d->AddRectFilled(ImVec2(x0 + 1, height - rule - 1), ImVec2(x1 - 1, height - 1), ImGui::GetColorU32(kGoldLine));
    const float y = (height - rule - titleSize.y) * 0.5f;
    d->AddText(g_fontHeading, g_fontHeading->FontSize, ImVec2(x0 + pad, y), ImGui::GetColorU32(kGold), title);
    d->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x0 + pad + titleSize.x + gap, y + (titleSize.y - keysSize.y) * 0.5f),
               ImGui::GetColorU32(Rgb(242, 240, 230)), keys);
}

// The Debug tab's previews, each window on its own.
void DrawPreviews()
{
    std::string words;
    {
        std::lock_guard lock(g_previewMutex);
        words.swap(g_previewWords);
    }
    std::istringstream in(words);
    for (std::string w; in >> w;) {
        Preview& p = g_preview;
        if (w == "hud") p.hud = true;
        else if (w == "strip") p.strip = true;
        else if (w == "desync") p.desync = true;
        else if (w == "p1") p.side[0] = true;
        else if (w == "p2") p.side[1] = true;
        else if (w == "vote") p.sides = 0;
        else if (w == "select") p.sides = 1;
        else if (w == "you1") p.you = 0;
        else if (w == "you2") p.you = 1;
        else if (w == "toasts") g_sampleToasts = true;
        else if (w == "relay") p.relayed = true;
        else if (w == "p2p") p.relayed = false;
        else if (w == "fade") {
            p.hud = true;
            p.fadeAt = GetTickCount64();
        }
        else if (w == "off") p.hud = p.strip = p.desync = p.side[0] = p.side[1] = false;
    }
    if (g_sampleToasts) {
        g_sampleToasts = false;
        AddToast("Player Two joined");
        AddToast("Player Two wants a rematch");
        AddToast("A spectator joined (1 watching)", kDim);
        AddToast("Session over: the other player left", kRed, 6);
    }
    const Preview& p = g_preview;
    if (!p.Any()) return;
    LoadTables();
    InitChoices();
    if (!g_tables.loaded) return;
    g_previewing = true;
    if (p.hud) {
        // "Fade out" plays the match's end for a moment, then they come back.
        const bool over = p.fadeAt && GetTickCount64() - p.fadeAt < kBarsFadeMs + 1200;
        DrawPlayerBars(PreviewView(over ? NetView::Phase::PostMatch : NetView::Phase::Running));
    }
    if (p.strip) DrawStrip(PreviewView(NetView::Phase::Running));
    if (p.desync) DrawDesyncBanner(PreviewView(NetView::Phase::Running));
    if (p.side[0] || p.side[1]) DrawSides(PreviewView(p.sides == 0 ? NetView::Phase::PostMatch : NetView::Phase::Lobby), p.side);
    g_previewing = false;
}

constexpr float kPanelFadeMs = 400;

void Draw(IDXGISwapChain* swapChain)
{
    if (swapChain != g_swapChain && !SetUp(swapChain)) return;
    std::vector<Message> input;
    {
        std::lock_guard lock(g_inputMutex);
        input.swap(g_input);
    }
    for (const Message& m : input) ImGui_ImplWin32_WndProcHandler(g_window, m.msg, m.wp, m.lp);
    ImGuiIO& io = ImGui::GetIO();
    FeedPads(io);
    // A frame every time, also with nothing to show: it consumes the input
    // events queued above, which would otherwise pile up.
    const NetView view = NetViewNow();
    const NetView::Phase phase = view.phase;
    g_inMatch = phase == NetView::Phase::Running;
    // A session's windows pick from the game's lists, also when the panel
    // was never opened.
    if (phase != NetView::Phase::Idle) {
        LoadTables();
        InitChoices();
    }
    // Reaching a session's lobby sets this player's input delay to the
    // suggestion for the ping between them (once a session; it can be
    // changed from there).
    static bool delaySet = false;
    if (phase == NetView::Phase::Idle || phase == NetView::Phase::Ended) {
        delaySet = false;
    }
    else if (!delaySet && phase == NetView::Phase::Lobby && view.linkPingMs && g_choices.initialised && !view.spectator) {
        delaySet = true;
        const int delay = SuggestedDelay(view.linkPingMs);
        g_choices.delay = delay;
        Act([delay] { NetSetDelay(static_cast<uint8_t>(delay)); });
        Log("overlay: input delay %d, suggested for a %u ms ping", delay, view.linkPingMs);
    }
    // After a match, over its results screen (the vote, then character
    // select's lobby): both sides' windows; they take the pads while this
    // player chooses.
    const bool sides = (phase == NetView::Phase::PostMatch || (phase == NetView::Phase::Lobby && view.overResults)) && !g_visible;
    g_voting = sides && !view.spectator;
    // Once the session goes into the battle the panel fades out and closes.
    static bool wasInBattle = false;
    static ULONGLONG panelFadeAt = 0;
    const bool inBattle = phase == NetView::Phase::Jumping || phase == NetView::Phase::Loading || phase == NetView::Phase::Running;
    if (inBattle && !wasInBattle && g_visible) panelFadeAt = GetTickCount64();
    wasInBattle = inBattle;
    if (!g_visible) panelFadeAt = 0;
    float panelAlpha = 1.0f;
    if (panelFadeAt) {
        const float t = static_cast<float>(GetTickCount64() - panelFadeAt) / kPanelFadeMs;
        if (t >= 1) {
            panelFadeAt = 0;
            SetOverlayVisible(false);
        }
        else {
            panelAlpha = 1 - t;
        }
    }
    const bool visible = g_visible;
    const bool session = phase != NetView::Phase::Idle;
    io.MouseDrawCursor = visible || g_voting; // the game hides the system cursor
    // Fonts for the game's resolution (1.0 at 720 lines): again when it
    // changed (last frame's size; the backend makes the new atlas).
    if (io.DisplaySize.y > 0 && std::abs(UiScaleFor(io.DisplaySize.y) - g_builtScale) > 0.05f) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        BuildFonts(UiScaleFor(io.DisplaySize.y));
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ToastChanges(view);
    // Not over a match: it waits for a menu.
    if (OnMenuScreen()) ToastUpdate();
    if (visible) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, panelAlpha);
        DrawPanel();
        ImGui::PopStyleVar();
    }
    else if (session) {
        const bool inMatch = phase == NetView::Phase::Running || phase == NetView::Phase::PostMatch;
        if (inMatch) DrawPlayerBars(view);
        else if (!sides) DrawStrip(view);
        if (sides) DrawSides(view);
    }
    if (session && view.desyncFrame >= 0 && (phase == NetView::Phase::Running || phase == NetView::Phase::PostMatch)) {
        DrawDesyncBanner(view);
    }
    const bool hint = !visible && (phase == NetView::Phase::Idle || phase == NetView::Phase::Ended) && OnMenuScreen();
    if (hint) DrawMenuHint();
    DrawPreviews();
    DrawToasts();
    const bool draw = visible || session || hint || !g_toasts.empty() || g_preview.Any();
    if (!draw || (!g_rtv && !CreateTarget(swapChain))) {
        ImGui::EndFrame();
        return;
    }
    ImGui::Render();

    // Draw on top of the finished frame, leaving the game's targets as they were.
    ID3D11RenderTargetView* saved = nullptr;
    ID3D11DepthStencilView* savedDepth = nullptr;
    g_context->OMGetRenderTargets(1, &saved, &savedDepth);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &saved, savedDepth);
    if (saved) saved->Release();
    if (savedDepth) savedDepth->Release();
}

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* self, UINT syncInterval, UINT flags)
{
    HookGuard guard;
    if (Stopping() || (flags & DXGI_PRESENT_TEST)) return g_origPresent(self, syncInterval, flags);
    // Frames per second: frames counted over each half second. (Averaging
    // 1/frame time read high whenever frame times were uneven, 61-62 at a
    // true 60: the mean of 1/t is above 1/mean t.)
    static LARGE_INTEGER last{}, freq{}, windowStart{};
    static int windowFrames = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    int64_t frameMicros = 0;
    if (last.QuadPart) {
        const double dt = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
        frameMicros = static_cast<int64_t>(dt * 1e6);
    }
    last = now;
    if (!windowStart.QuadPart) windowStart = now;
    windowFrames++;
    const double window = double(now.QuadPart - windowStart.QuadPart) / double(freq.QuadPart);
    if (window >= 0.5) {
        // Frames presented since the window began, over its length.
        const float rate = static_cast<float>((windowFrames - 1) / window);
        g_fps = g_fps > 0 && window < 2 ? g_fps * 0.5f + rate * 0.5f : rate;
        windowStart = now;
        windowFrames = 1;
    }
    {
        hitch::Scope timed(hitch::Part::Overlay);
        Draw(self);
    }
    HRESULT result;
    {
        hitch::Scope timed(hitch::Part::Present);
        const auto from = std::chrono::steady_clock::now();
        result = g_origPresent(self, g_noVsync ? 0 : syncInterval, flags);
        g_presentMicros += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - from).count();
        g_presentFrames++;
    }
    g_gameSyncInterval = syncInterval;
    g_gamePresentFlags = flags;
    // The frame-drop log (hitch.h), for the frame that ends here.
    if (frameMicros) hitch::FramePresented(frameMicros, g_inMatch);
    return result;
}

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                              UINT flags)
{
    HookGuard guard;
    // The back buffer goes away: our view of it must be gone first.
    if (self == g_swapChain) ReleaseTarget();
    return g_origResizeBuffers(self, count, width, height, format, flags);
}

// Present and ResizeBuffers addresses, from a throwaway device and swap chain
// on a hidden window.
bool FindSwapChainFunctions()
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SfvRollbackOverlayProbe";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance,
                             nullptr);
    if (!w) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = w;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                     D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context);
    if (SUCCEEDED(hr) && swapChain) {
        void** vtable = *reinterpret_cast<void***>(swapChain);
        g_presentTarget = vtable[kPresentSlot];
        g_resizeTarget = vtable[kResizeBuffersSlot];
    }
    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return g_presentTarget && g_resizeTarget;
}

} // namespace

bool InstallOverlay(const std::wstring& panelPath)
{
    g_panelPath = panelPath;
    if (!FindSwapChainFunctions()) {
        Log("overlay: no D3D11 swap chain functions");
        return false;
    }
    const bool ok = MH_CreateHook(g_presentTarget, reinterpret_cast<void*>(&PresentDetour),
                                  reinterpret_cast<void**>(&g_origPresent)) == MH_OK &&
                    MH_CreateHook(g_resizeTarget, reinterpret_cast<void*>(&ResizeBuffersDetour),
                                  reinterpret_cast<void**>(&g_origResizeBuffers)) == MH_OK &&
                    MH_EnableHook(g_presentTarget) == MH_OK && MH_EnableHook(g_resizeTarget) == MH_OK;
    Log("overlay %s (F1 or Back + Start)", ok ? "installed" : "NOT installed");
    return ok;
}

void ShutdownOverlay()
{
    g_visible = false;
    ReleaseImGui();
}

void SetOverlayVisible(bool visible)
{
    if (visible && !g_visible) g_focusPanel = true;
    if (g_visible.exchange(visible) != visible) Log("overlay %s", visible ? "shown" : "hidden");
}

void ToggleOverlay()
{
    SetOverlayVisible(!g_visible);
}

bool OverlayVisible()
{
    return g_visible;
}

bool OverlayTakesInput()
{
    return g_visible || g_voting;
}

void SetVsync(bool on)
{
    g_noVsync = !on;
    Log("overlay: vsync %s", on ? "on (the game's own)" : "off (present with sync interval 0)");
}

bool VsyncOn() { return !g_noVsync; }

std::string VsyncInfo()
{
    // The time in Present since the last call, per frame.
    static int64_t lastMicros = 0, lastFrames = 0;
    const int64_t micros = g_presentMicros, frames = g_presentFrames;
    const int64_t count = frames - lastFrames;
    const double perFrame = count > 0 ? double(micros - lastMicros) / double(count) / 1000.0 : 0;
    lastMicros = micros;
    lastFrames = frames;
    char line[256];
    DXGI_SWAP_CHAIN_DESC desc{};
    const bool haveDesc = g_swapChain && SUCCEEDED(g_swapChain->GetDesc(&desc));
    std::snprintf(line, sizeof(line),
                  "vsync %s, the game's sync interval %u flags %x, swap effect %d, %u buffers, %s, %.2f ms in Present over %lld frames, %.1f fps",
                  g_noVsync ? "off" : "on", g_gameSyncInterval.load(), g_gamePresentFlags.load(), haveDesc ? int(desc.SwapEffect) : -1,
                  haveDesc ? desc.BufferCount : 0, haveDesc ? (desc.Windowed ? "windowed" : "fullscreen") : "?", perFrame,
                  static_cast<long long>(count), RenderFps());
    return line;
}

void PreviewOverlay(const std::string& words)
{
    std::lock_guard lock(g_previewMutex);
    g_previewWords += " " + words;
}

void SetOverlayBuild(const std::string& version)
{
    g_build = version;
}

float RenderFps()
{
    return g_fps;
}

bool OverlayWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_F1 && !(lp & (1 << 30))) { // not on key repeat
        ToggleOverlay();
        return true;
    }
    if (w != g_inputWindow) return false;
    const bool keyboard = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
    const bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
    // Not WM_MOUSELEAVE: ImGui's backend asks for it with TrackMouseEvent,
    // which from the render thread reports a leave at once, and after a
    // leave it takes the position from the real cursor instead of the
    // messages. Positions come from WM_MOUSEMOVE only.
    const bool relevant = keyboard || mouse || msg == WM_SETFOCUS || msg == WM_KILLFOCUS;
    if (relevant && msg != WM_INPUT) {
        std::lock_guard lock(g_inputMutex);
        if (g_input.size() < 1024) g_input.push_back({msg, wp, lp});
    }
    // Open: the game sees no keyboard or mouse (WM_INPUT is its raw mouse).
    return (g_visible || g_voting) && (keyboard || mouse);
}

void OverlayPad(unsigned user, const XINPUT_GAMEPAD& pad)
{
    if (user >= XUSER_MAX_COUNT) return;
    g_pads[user].buttons = pad.wButtons;
    g_pads[user].lx = pad.sThumbLX;
    g_pads[user].ly = pad.sThumbLY;
    const uint16_t buttons = pad.wButtons;
    constexpr uint16_t kChord = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
    const bool now = (buttons & kChord) == kChord;
    const bool before = (g_lastButtons[user] & kChord) == kChord;
    g_lastButtons[user] = buttons;
    if (now && !before) ToggleOverlay();
}

} // namespace sfv::mod
