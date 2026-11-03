// GUI 版エントリーポイント: Win32 + Direct3D 11 + Dear ImGui
// GPU が使えない環境 (VM / リモートデスクトップ / 古いドライバ) でも WARP (ソフトウェア描画) で起動する。
#include "core/Log.h"
#include "editor/Editor.h"
#include "editor/Icons.h"
#include "io/ImageIO.h"
#include "util/CrashHandler.h"
#include "util/FileUtil.h"

#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resizeW = 0, g_resizeH = 0;
bool g_occluded = false;
std::string g_gpuInfo;
Editor* g_editor = nullptr;

std::string Narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

void Fatal(const std::string& msg) {
    LOG_ERROR("%s", msg.c_str());
    std::wstring w = Widen(msg + "\n\nログ: " + FileUtil::Join(FileUtil::AppDataDir(), "ModelEditor_log.txt"));
    MessageBoxW(nullptr, w.c_str(), L"ModelEditor", MB_OK | MB_ICONERROR);
}

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void CleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

void CleanupDevice() {
    CleanupRenderTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->ClearState(); g_ctx->Flush(); g_ctx->Release(); g_ctx = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

// ハードウェア → WARP の順で、スワップチェーンは FLIP_DISCARD → DISCARD の順に試す
bool CreateDevice(HWND hwnd, bool forceWarp) {
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    struct Attempt { D3D_DRIVER_TYPE driver; DXGI_SWAP_EFFECT effect; };
    std::vector<Attempt> attempts;
    if (!forceWarp) {
        attempts.push_back({ D3D_DRIVER_TYPE_HARDWARE, DXGI_SWAP_EFFECT_FLIP_DISCARD });
        attempts.push_back({ D3D_DRIVER_TYPE_HARDWARE, DXGI_SWAP_EFFECT_DISCARD });
    }
    attempts.push_back({ D3D_DRIVER_TYPE_WARP, DXGI_SWAP_EFFECT_FLIP_DISCARD });
    attempts.push_back({ D3D_DRIVER_TYPE_WARP, DXGI_SWAP_EFFECT_DISCARD });

    for (const Attempt& a : attempts) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = a.effect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? 2 : 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = a.effect;
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, a.driver, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_ctx);
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) // デバッグレイヤーが無い環境
            hr = D3D11CreateDeviceAndSwapChain(nullptr, a.driver, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_ctx);
        if (FAILED(hr)) {
            LOG_WARN("D3D11 デバイス作成失敗 (driver=%d swap=%d hr=0x%08x)。次の方法を試します", (int)a.driver, (int)a.effect, (unsigned)hr);
            continue;
        }
        std::string adapter = a.driver == D3D_DRIVER_TYPE_WARP ? "WARP (ソフトウェア描画)" : "?";
        IDXGIDevice* dxgiDev = nullptr;
        if (a.driver == D3D_DRIVER_TYPE_HARDWARE && SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&ad))) {
                DXGI_ADAPTER_DESC desc;
                if (SUCCEEDED(ad->GetDesc(&desc)))
                    adapter = Narrow(desc.Description) + " (VRAM " + std::to_string(desc.DedicatedVideoMemory / (1024 * 1024)) + " MB)";
                ad->Release();
            }
            dxgiDev->Release();
        }
        const char* fl = got == D3D_FEATURE_LEVEL_11_0 ? "11_0" : got == D3D_FEATURE_LEVEL_10_1 ? "10_1" : "10_0";
        g_gpuInfo = "Direct3D 11 / 機能レベル " + std::string(fl) + " / " + adapter;
        // Alt+Enter の全画面切替は無効化
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(g_swap->GetParent(IID_PPV_ARGS(&factory)))) {
            factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            factory->Release();
        }
        CreateRenderTarget();
        LOG_INFO("GPU: %s", g_gpuInfo.c_str());
        if (a.driver == D3D_DRIVER_TYPE_WARP) LOG_WARN("GPU を使用できないためソフトウェア描画 (WARP) で動作しています。表示が遅い場合があります");
        return true;
    }
    return false;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_resizeW = (UINT)LOWORD(lParam);
        g_resizeH = (UINT)HIWORD(lParam);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 640;
        mmi->ptMinTrackSize.y = 400;
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* r = (const RECT*)lParam;
        SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wParam;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::string> files;
        for (UINT i = 0; i < count; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring w(len + 1, L'\0');
            DragQueryFileW(drop, i, w.data(), len + 1);
            w.resize(len);
            files.push_back(Narrow(w.c_str()));
        }
        DragFinish(drop);
        if (g_editor) g_editor->OnDropFiles(files);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // Alt 単体でメニューに入らない (Alt+ドラッグでカメラ操作するため)
        break;
    case WM_CLOSE:
        if (g_editor) { g_editor->RequestQuit(); return 0; }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// 本文フォント + アイコンフォント (Segoe Fluent Icons / MDL2) を合成したフォントを 1 つ作る
ImFont* AddUiFont(const std::string& fontsDir, float size, bool bold) {
    ImGuiIO& io = ImGui::GetIO();
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        b.AddText("◆◇■□○●★☆→←↑↓×…°①②③④⑤ⅠⅡⅢ▶▼✓▣");
        b.BuildRanges(&ranges);
    }
    // 読みやすさ優先: BIZ UDPゴシック (ユニバーサルデザイン) → Yu Gothic UI → Meiryo UI → 旧来のフォント
    struct Candidate { const char* file; int face; };
    const Candidate regular[] = { { "BIZ-UDGothicR.ttc", 1 }, { "YuGothM.ttc", 1 }, { "meiryo.ttc", 2 }, { "meiryo.ttc", 0 }, { "msgothic.ttc", 0 } };
    const Candidate boldFaces[] = { { "BIZ-UDGothicB.ttc", 1 }, { "YuGothB.ttc", 1 }, { "meiryob.ttc", 2 } };
    ImFont* font = nullptr;
    auto tryList = [&](const Candidate* list, size_t n) {
        for (size_t i = 0; i < n && !font; ++i) {
            std::string path = fontsDir + list[i].file;
            if (!FileUtil::Exists(path)) continue;
            ImFontConfig cfg;
            cfg.OversampleH = 3;
            cfg.OversampleV = 1;
            cfg.FontNo = list[i].face;
            font = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg, ranges.Data);
            if (font) LOG_INFO("フォント: %s #%d (%.0fpx)", path.c_str(), list[i].face, size);
        }
    };
    if (bold) tryList(boldFaces, std::size(boldFaces));
    tryList(regular, std::size(regular));
    if (!font) return nullptr;
    // アイコン (私用領域 U+E700〜U+F8FF) を同じフォントに合成する
    static const ImWchar iconRanges[] = { 0xE700, 0xF8FF, 0 };
    for (const char* f : { "SegoeIcons.ttf", "segmdl2.ttf" }) {
        std::string path = fontsDir + f;
        if (!FileUtil::Exists(path)) continue;
        ImFontConfig ic;
        ic.MergeMode = true;
        ic.PixelSnapH = true;
        ic.GlyphMinAdvanceX = size; // アイコンの幅をそろえる
        ic.GlyphOffset = ImVec2(0, std::round(size * 0.12f));
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), size * 0.95f, &ic, iconRanges)) {
            Icons::loaded = true;
            break;
        }
    }
    return font;
}

void LoadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear(); // 大きさを変えて作り直すとき用
    Icons::loaded = false;
    Icons::large = nullptr;
    wchar_t winDir[MAX_PATH] = L"C:\\Windows";
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::string fonts = Narrow(winDir) + "\\Fonts\\";
    if (AddUiFont(fonts, std::round(15.0f * scale), false)) {
        Icons::large = AddUiFont(fonts, std::round(22.0f * scale), true);
        if (!Icons::loaded) LOG_WARN("アイコンフォント (Segoe Fluent Icons / MDL2) が見つかりません。ボタンは文字で表示します");
        return;
    }
    LOG_WARN("日本語フォントが見つかりませんでした。既定フォントを使用します (日本語は表示されません)");
    ImFontConfig def;
    def.SizePixels = std::round(13.0f * scale);
    io.Fonts->AddFontDefault(&def);
}

// Unreal Engine 5 に近い配色: 落ち着いたグレーの濃淡で領域を分け、青は「選択中・オン」だけに使う
void ApplyStyle(float scale) {
    ImGui::GetStyle() = ImGuiStyle(); // ScaleAllSizes は掛け算なので、作り直すときは既定から始める
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 4;
    s.FrameRounding = 4;
    s.PopupRounding = 6;
    s.GrabRounding = 4;
    s.TabRounding = 4;
    s.ScrollbarRounding = 6;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 1;
    s.PopupBorderSize = 1;
    s.TabBorderSize = 0;
    s.TabBarBorderSize = 2;
    s.TabBarOverlineSize = 2;
    s.WindowPadding = ImVec2(10, 8);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(6, 3);
    s.IndentSpacing = 16;
    s.ScrollbarSize = 13;
    s.GrabMinSize = 10;
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(10, 4);
    s.WindowMenuButtonPosition = ImGuiDir_None; // タブ左の ▼ ボタンは使わない (分かりにくい)
    s.DockingSeparatorSize = 3;

    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    const ImVec4 bg = rgb(36, 36, 36);          // パネル
    const ImVec4 bgDark = rgb(21, 21, 21);      // タブバー / タイトル
    const ImVec4 bgDeep = rgb(15, 15, 15);      // 入力欄
    const ImVec4 bgRaised = rgb(47, 47, 47);    // ボタン / 見出し
    const ImVec4 hover = rgb(64, 64, 64);
    const ImVec4 border = rgb(58, 58, 58);
    const ImVec4 accent = rgb(0, 112, 224);     // 選択 (UE の青)
    const ImVec4 accentHover = rgb(30, 136, 245);
    const ImVec4 text = rgb(214, 214, 214);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = rgb(128, 128, 128);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = rgb(30, 30, 30);
    c[ImGuiCol_PopupBg] = rgb(30, 30, 30, 0.98f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = bgDeep;
    c[ImGuiCol_FrameBgHovered] = rgb(26, 26, 26);
    c[ImGuiCol_FrameBgActive] = rgb(32, 32, 32);
    c[ImGuiCol_TitleBg] = bgDark;
    c[ImGuiCol_TitleBgActive] = bgDark;
    c[ImGuiCol_TitleBgCollapsed] = bgDark;
    c[ImGuiCol_MenuBarBg] = bgDark;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(70, 70, 70);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(95, 95, 95);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(120, 120, 120);
    c[ImGuiCol_CheckMark] = accentHover;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHover;
    c[ImGuiCol_Button] = bgRaised;
    c[ImGuiCol_ButtonHovered] = hover;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = rgb(0, 112, 224, 0.45f);
    c[ImGuiCol_HeaderHovered] = rgb(255, 255, 255, 0.07f);
    c[ImGuiCol_HeaderActive] = rgb(0, 112, 224, 0.65f);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accentHover;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_ResizeGripActive] = accentHover;
    c[ImGuiCol_Tab] = bgDark;
    c[ImGuiCol_TabHovered] = hover;
    c[ImGuiCol_TabSelected] = bg;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bgDark;
    c[ImGuiCol_TabDimmedSelected] = bg;
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(90, 90, 90);
    c[ImGuiCol_DockingPreview] = rgb(0, 112, 224, 0.6f);
    c[ImGuiCol_DockingEmptyBg] = bgDark;
    c[ImGuiCol_TableHeaderBg] = bgRaised;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = rgb(48, 48, 48);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
    c[ImGuiCol_TextSelectedBg] = rgb(0, 112, 224, 0.45f);
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotLines] = accentHover;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    s.ScaleAllSizes(scale);
}

// UI の大きさ = DPI × 環境設定の倍率 × ウィンドウの大きさに合わせた係数。
// 1600 x 900 (DPI 100% 換算) を基準に、ウィンドウが小さいほど文字とボタンを小さくして、パネルの枠内に収める
float ComputeUiScale(HWND hwnd, float userScale) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const float dpi = std::max(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd), 0.5f);
    const float w = (float)(rc.right - rc.left) / dpi, h = (float)(rc.bottom - rc.top) / dpi;
    float fit = 1.0f;
    if (w > 0 && h > 0) fit = std::clamp(std::min(w / 1600.0f, h / 900.0f), 0.6f, 1.0f);
    float s = dpi * userScale * fit;
    return std::round(s * 20.0f) / 20.0f; // 0.05 刻み (少しのリサイズで毎回作り直さない)
}

float ReadUiScaleSetting() {
    std::vector<uint8_t> data;
    if (FileUtil::ReadBinary(FileUtil::Join(FileUtil::AppDataDir(), "ModelEditor_settings.ini"), data)) {
        std::string s(data.begin(), data.end());
        size_t p = s.find("ui_scale=");
        if (p != std::string::npos) return std::clamp((float)atof(s.c_str() + p + 9), 0.5f, 3.0f);
    }
    return 1.0f;
}

bool SaveBackbuffer(const std::string& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &staging))) {
        g_ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) {
            ImageData img;
            img.width = (int)td.Width;
            img.height = (int)td.Height;
            img.rgba.resize((size_t)img.width * img.height * 4);
            for (int y = 0; y < img.height; ++y)
                memcpy(&img.rgba[(size_t)y * img.width * 4], (uint8_t*)ms.pData + (size_t)y * ms.RowPitch, (size_t)img.width * 4);
            for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
            g_ctx->Unmap(staging, 0);
            ok = ImageIO::SavePNG(path, img);
        }
        staging->Release();
    }
    back->Release();
    return ok;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CrashHandler::Install(true);
    Log::Get().SetEchoStdout(false);
    Log::Get().SetFile(FileUtil::Join(FileUtil::AppDataDir(), "ModelEditor_log.txt"));
    ImGui_ImplWin32_EnableDpiAwareness();

    std::vector<std::string> args;
    {
        int n = 0;
        LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 0; i < n; ++i) args.push_back(Narrow(w[i]));
        LocalFree(w);
    }
    // 引数: 開くファイル / デバッグ用オプション
    std::string screenshotPath, selectName;
    std::vector<std::string> openFiles; // 複数指定するとすべて読み込む (1 つ目で開き、残りは追加)
    std::vector<std::string> focusWindows;
    int screenshotFrames = 90, renderMode = -1;
    int winW = 1600, winH = 900;            // --size W H: ウィンドウの大きさ (確認用)
    float mouseX = -1.0f, mouseY = -1.0f;  // --mouse X Y: マウスをその位置に置いたことにする (ツールチップの確認用)
    bool playOnLoad = false, smoke = false, forceWarp = false, addRuler = false;
    float rulerHeight = 0.0f;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--screenshot") screenshotPath = next();
        else if (a == "--frames") screenshotFrames = atoi(next().c_str());
        else if (a == "--select") selectName = next();
        else if (a == "--focus") focusWindows.push_back(next());
        else if (a == "--mode") renderMode = atoi(next().c_str());
        else if (a == "--play") playOnLoad = true;
        else if (a == "--smoketest") smoke = true;
        else if (a == "--warp") forceWarp = true; // ソフトウェア描画を強制 (動作確認用)
        else if (a == "--ruler") { rulerHeight = (float)atof(next().c_str()); addRuler = true; }
        else if (a == "--size") { winW = atoi(next().c_str()); winH = atoi(next().c_str()); }
        else if (a == "--mouse") { mouseX = (float)atof(next().c_str()); mouseY = (float)atof(next().c_str()); }
        else if (!a.empty() && a[0] != '-') openFiles.push_back(a);
    }

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, LoadIconW(nullptr, IDI_APPLICATION),
                       LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"ModelEditorWindow", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ModelEditor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        Fatal("ウィンドウを作成できませんでした");
        return 1;
    }
    if (!CreateDevice(hwnd, forceWarp)) {
        CleanupDevice();
        DestroyWindow(hwnd);
        Fatal("Direct3D 11 を初期化できませんでした。\nWindows Update でグラフィックスドライバーを更新してからお試しください。");
        return 1;
    }
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, (screenshotPath.empty() && !smoke) ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    static std::string iniPath = FileUtil::Join(FileUtil::AppDataDir(), "ModelEditor_imgui.ini");
    io.IniFilename = iniPath.c_str();

    float scale = ComputeUiScale(hwnd, ReadUiScaleSetting());
    LoadFonts(scale);
    ApplyStyle(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_ctx);

    Editor editor;
    g_editor = &editor;
    if (!editor.Init(hwnd, g_device, g_ctx, g_gpuInfo)) {
        Fatal("描画の初期化に失敗しました (シェーダーを作成できません)。");
        return 1;
    }
    if (!selectName.empty()) editor.SetSelectAfterLoad(selectName);
    for (auto& w : focusWindows) editor.FocusWindowLater(w);
    if (renderMode >= 0) editor.SetRenderMode(renderMode);
    editor.PlayAnimationOnLoad(playOnLoad);
    if (smoke) editor.EnableSmokeTest();
    if (addRuler) editor.AddRulerAfterLoad(rulerHeight);
    for (size_t i = 0; i < openFiles.size(); ++i) editor.OpenFile(openFiles[i], i > 0);

    int frameCount = 0;
    bool running = true;
    while (running && !editor.WantsQuit()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(hwnd) || (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
            Sleep(16);
            continue;
        }
        g_occluded = false;
        if (g_resizeW != 0 && g_resizeH != 0) {
            CleanupRenderTarget();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }
        // ウィンドウの大きさ・DPI・環境設定の倍率が変わったら、UI (フォントと余白) をその大きさで作り直す
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float want = ComputeUiScale(hwnd, editor.UiScaleSetting());
            if (std::abs(want - scale) > 0.01f) {
                scale = want;
                LoadFonts(scale);
                ApplyStyle(scale);
                ImGui_ImplDX11_InvalidateDeviceObjects(); // 次の NewFrame でフォントのテクスチャを作り直す
                LOG_INFO("UI の大きさを x%.2f にしました (ウィンドウの大きさに合わせて)", scale);
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (mouseX >= 0.0f) ImGui::GetIO().AddMousePosEvent(mouseX, mouseY);
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        editor.Frame();
        ImGui::Render();

        const float clear[4] = { 0.08f, 0.08f, 0.09f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (!screenshotPath.empty() && ++frameCount >= screenshotFrames) {
            if (SaveBackbuffer(screenshotPath)) LOG_INFO("スクリーンショットを保存: %s", screenshotPath.c_str());
            break;
        }

        HRESULT hr = g_swap->Present(1, 0);
        g_occluded = hr == DXGI_STATUS_OCCLUDED;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || editor.RendererLost()) {
            // ドライバ更新・TDR 等で GPU を失った場合は作り直して続行する
            HRESULT reason = g_device ? g_device->GetDeviceRemovedReason() : hr;
            LOG_ERROR("GPU デバイスが失われました (0x%08x)。再初期化します", (unsigned)reason);
            ImGui_ImplDX11_Shutdown();
            CleanupDevice();
            if (!CreateDevice(hwnd, false) || !ImGui_ImplDX11_Init(g_device, g_ctx) || !editor.ResetRenderer(g_device, g_ctx, g_gpuInfo)) {
                Fatal("GPU デバイスの再初期化に失敗しました。アプリケーションを終了します。");
                break;
            }
        }
    }

    int exitCode = smoke ? (editor.SmokeTestErrors() ? 5 : 0) : 0;
    editor.Shutdown();
    g_editor = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return exitCode;
}
