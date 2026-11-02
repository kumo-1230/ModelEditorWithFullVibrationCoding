// GUI 版エントリーポイント: Win32 + Direct3D 11 + Dear ImGui
// GPU が使えない環境 (VM / リモートデスクトップ / 古いドライバ) でも WARP (ソフトウェア描画) で起動する。
#include "core/Log.h"
#include "editor/Editor.h"
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

void LoadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const float size = std::round(16.0f * scale);
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        b.AddText("◆◇■□○●★☆→←↑↓×…°①②③④⑤ⅠⅡⅢ");
        b.BuildRanges(&ranges);
    }
    // Windows 標準の日本語フォントを順に探す
    wchar_t winDir[MAX_PATH] = L"C:\\Windows";
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::string fonts = Narrow(winDir) + "\\Fonts\\";
    const char* candidates[] = { "meiryo.ttc", "YuGothM.ttc", "YuGothR.ttc", "msgothic.ttc", "BIZ-UDGothicR.ttc", "meiryob.ttc" };
    for (const char* f : candidates) {
        std::string path = fonts + f;
        if (FileUtil::Exists(path) && io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg, ranges.Data)) {
            LOG_INFO("フォント: %s (%.0fpx)", path.c_str(), size);
            return;
        }
    }
    LOG_WARN("日本語フォントが見つかりませんでした。既定フォントを使用します (日本語は表示されません)");
    ImFontConfig def;
    def.SizePixels = std::round(13.0f * scale);
    io.Fonts->AddFontDefault(&def);
}

void ApplyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 4;
    s.FrameRounding = 3;
    s.GrabRounding = 3;
    s.TabRounding = 3;
    s.ScrollbarRounding = 4;
    s.WindowBorderSize = 1;
    s.FramePadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(7, 4);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.14f, 0.15f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.13f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.26f, 0.42f, 0.62f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.50f, 0.75f, 0.80f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.29f, 0.45f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.17f, 0.20f, 0.24f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.24f, 0.38f, 0.56f, 1.0f);
    s.ScaleAllSizes(scale);
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
    std::string screenshotPath, selectName, openFile;
    std::vector<std::string> focusWindows;
    int screenshotFrames = 90, renderMode = -1;
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
        else if (!a.empty() && a[0] != '-' && openFile.empty()) openFile = a;
    }

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, LoadIconW(nullptr, IDI_APPLICATION),
                       LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"ModelEditorWindow", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ModelEditor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1600, 900,
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

    float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd) * ReadUiScaleSetting();
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
    if (!openFile.empty()) editor.OpenFile(openFile);

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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
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
