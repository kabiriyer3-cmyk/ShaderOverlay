// ShaderOverlay 1.2: post-processing overlay for games without ReShade support.
// No injection: captures the monitor (DXGI Desktop Duplication), runs an HLSL
// effect pass in D3D11, and presents it in a topmost, click-through window
// that is excluded from capture (so it never re-processes itself).
// The menu is a full-desktop, semi-transparent, always-on-top window.
// Requirements: Windows 10 2004+ / Windows 11, game in BORDERLESS / WINDOWED mode.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>
#include <gdiplus.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <dwmapi.h>
#if defined(_MSC_VER)
  // Windows.Graphics.Capture (window capture that ignores our overlay) - MSVC / Windows SDK only
  #define SO_HAS_WGC 1
  #include <roapi.h>
  #include <wrl/wrappers/corewrappers.h>
  #include <windows.foundation.h>
  #include <windows.graphics.capture.h>
  #include <windows.graphics.capture.interop.h>
  #include <windows.graphics.directx.direct3d11.interop.h>
  #include <d3d11_4.h>
  #pragma comment(lib, "runtimeobject.lib")
#else
  #define SO_HAS_WGC 0
#endif
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdlib>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#endif

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

using Microsoft::WRL::ComPtr;
using namespace Gdiplus;

// ---------------------------------------------------------------- effect params
struct Params {
    float texelW, texelH;
    float sharpen, vibrance, saturation, contrast, brightness, gamma, temperature, vignette;
    float pad0, pad1;   // 48 bytes
};

struct SliderDef { const wchar_t* key; const wchar_t* label; float Params::*field; float mn, mx, def; };
static const SliderDef kSliders[] = {
    { L"Sharpen",     L"Sharpen",     &Params::sharpen,     0.0f,  1.0f,  0.50f },
    { L"Vibrance",    L"Vibrance",    &Params::vibrance,   -1.0f,  1.0f,  0.25f },
    { L"Saturation",  L"Saturation",  &Params::saturation,  0.0f,  2.0f,  1.00f },
    { L"Contrast",    L"Contrast",    &Params::contrast,    0.5f,  1.5f,  1.00f },
    { L"Brightness",  L"Brightness",  &Params::brightness, -0.25f, 0.25f, 0.00f },
    { L"Gamma",       L"Gamma",       &Params::gamma,       0.5f,  2.0f,  1.00f },
    { L"Temperature", L"Temperature", &Params::temperature,-1.0f,  1.0f,  0.00f },
    { L"Vignette",    L"Vignette",    &Params::vignette,    0.0f,  1.0f,  0.00f },
};
static const int kNumSliders = sizeof(kSliders) / sizeof(kSliders[0]);

struct Hotkey { const wchar_t* key; const wchar_t* label; int id; UINT mod; UINT vk; };
static Hotkey g_hotkeys[] = {
    { L"ToggleEffects", L"Toggle effects",     1, 0,           VK_F8  },
    { L"ToggleMenu",    L"Open / close menu",  2, 0,           VK_F9  },
    { L"Quit",          L"Quit ShaderOverlay", 3, MOD_CONTROL, VK_F12 },
    { L"ReShadeInput",  L"ReShade input mode (focus overlay)", 4, 0, VK_F10 },
};
static const int kNumHotkeys = sizeof(g_hotkeys) / sizeof(g_hotkeys[0]);

#define WM_APP_TRAY (WM_APP + 1)

// ---------------------------------------------------------------- globals
static ComPtr<IDXGIFactory1>          g_factory;
static ComPtr<IDXGIAdapter1>          g_adapter;
static ComPtr<IDXGIOutput1>           g_output;
static ComPtr<ID3D11Device>           g_dev;
static ComPtr<ID3D11DeviceContext>    g_ctx;
static ComPtr<IDXGIOutputDuplication> g_dup;
static ComPtr<IDXGISwapChain>         g_swap;
static ComPtr<ID3D11RenderTargetView> g_rtv;
static ComPtr<ID3D11Texture2D>        g_capTex;
static ComPtr<ID3D11ShaderResourceView> g_capSrv;
static ComPtr<ID3D11VertexShader>     g_vs;
static ComPtr<ID3D11PixelShader>      g_ps;
static ComPtr<ID3D11Buffer>           g_cb;
static ComPtr<ID3D11SamplerState>     g_smp;

static Params  g_p{};
static RECT    g_mon{};
static HWND    g_overlay = nullptr, g_menu = nullptr, g_prevFg = nullptr;
static bool    g_interactive = false;
// experimental: stream mode (screen share can see the effects)
static bool    g_streamMode = false;
static HWND    g_streamTarget = nullptr;
static std::wstring g_streamInfo;
static int     g_outW = 0, g_outH = 0;          // current overlay/swap chain size
static RECT    g_outRect{};
// easter egg pet
static bool    g_petUnlocked = false, g_petOn = false;
static float   g_petX = 0, g_petDir = 1;
static int     g_petFrame = 0, g_petJump = 0, g_petBlink = 0, g_petHeart = 0, g_versionClicks = 0;
static DWORD   g_versionClickT = 0, g_petTick = 0;
static std::wstring g_typed;
static bool    g_effectsOn = true, g_running = true, g_dirty = true, g_haveFrame = false;
static int     g_monitorIndex = 0;
static std::wstring g_ini;

// UI state
enum { H_PANEL = 1, H_CLOSE, H_NAV, H_TOGGLE, H_SLIDER, H_RESET, H_SAVE, H_HOTKEY, H_STREAM, H_PET, H_VERSION, H_CRAB };
struct Hit { float x, y, w, h; int type, idx; };
static std::vector<Hit> g_hits;
static float   g_s = 1.0f;                 // DPI scale
static bool    g_uiDirty = true;
static int     g_page = 0, g_drag = -1, g_capturing = -1, g_hoverType = 0, g_hoverIdx = -1;
static float   g_tx0 = 0, g_tx1 = 1;       // slider track extents
static std::wstring g_status;
static HDC     g_memDC = nullptr;
static HBITMAP g_dib = nullptr;
static void*   g_bits = nullptr;
static Bitmap* g_bmp = nullptr;
static Font   *fTitle, *fBody, *fBold, *fSmall, *fIcon;
static ULONG_PTR g_gdipToken = 0;
static NOTIFYICONDATAW g_nid{};

static inline float S(float v) { return v * g_s; }

// ---------------------------------------------------------------- shader
static const char* kHLSL = R"(
cbuffer P : register(b0) {
    float2 texel; float sharpen; float vibrance;
    float saturation; float contrast; float brightness; float gamma;
    float temperature; float vignette; float2 pad;
};
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 S(float2 uv, float2 off) { return tex.SampleLevel(smp, uv + off * texel, 0).rgb; }
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

float4 PSMain(VSOut i) : SV_Target {
    float3 c = S(i.uv, float2(0, 0));

    // Contrast Adaptive Sharpening (simplified CAS)
    if (sharpen > 0.001) {
        float3 n = S(i.uv, float2(0,-1)), s = S(i.uv, float2(0,1));
        float3 e = S(i.uv, float2(1, 0)), w = S(i.uv, float2(-1,0));
        float3 mn = min(c, min(min(n, s), min(e, w)));
        float3 mx = max(c, max(max(n, s), max(e, w)));
        float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
        float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, saturate(sharpen)));
        c = saturate((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt));
    }

    c += brightness;
    c = saturate((c - 0.5) * contrast + 0.5);
    c = pow(c, 1.0 / max(gamma, 0.01));

    float l = dot(c, LUMA);
    c = lerp(l.xxx, c, saturation);

    float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    l = dot(c, LUMA);
    c = lerp(l.xxx, c, 1.0 + vibrance * (1.0 - sat));

    c.r += temperature * 0.08;
    c.b -= temperature * 0.08;

    if (vignette > 0.001) {
        float2 d = i.uv - 0.5;
        c *= saturate(1.0 - dot(d, d) * vignette * 1.5);
    }
    return float4(saturate(c), 1.0);
}
)";

// ---------------------------------------------------------------- helpers
static void Fail(const wchar_t* msg, HRESULT hr = S_OK) {
    wchar_t buf[512];
    swprintf(buf, 512, L"%ls\n\nHRESULT: 0x%08X", msg, (unsigned)hr);
    MessageBoxW(nullptr, buf, L"ShaderOverlay", MB_ICONERROR);
    ExitProcess(1);
}

static std::wstring KeyName(UINT vk) {
    wchar_t b[64];
    if (vk >= VK_F1 && vk <= VK_F24) { swprintf(b, 64, L"F%u", vk - VK_F1 + 1); return b; }
    UINT sc = MapVirtualKeyW(vk, 0 /*MAPVK_VK_TO_VSC*/);
    bool ext = false;
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK: ext = true;
    }
    LONG lp = (LONG)((sc & 0xFF) << 16) | (ext ? (1 << 24) : 0);
    if (sc && GetKeyNameTextW(lp, b, 64) > 0) return b;
    swprintf(b, 64, L"Key 0x%02X", vk);
    return b;
}

static std::wstring VkName(UINT mod, UINT vk) {
    std::wstring s;
    if (mod & MOD_CONTROL) s += L"Ctrl + ";
    if (mod & MOD_ALT)     s += L"Alt + ";
    if (mod & MOD_SHIFT)   s += L"Shift + ";
    return s + KeyName(vk);
}

// ---------------------------------------------------------------- ini
static void ResetParams() {
    for (int i = 0; i < kNumSliders; ++i) g_p.*(kSliders[i].field) = kSliders[i].def;
    g_dirty = g_uiDirty = true;
}

static void SaveIni() {
    wchar_t b[64];
    for (int i = 0; i < kNumSliders; ++i) {
        swprintf(b, 64, L"%.3f", g_p.*(kSliders[i].field));
        WritePrivateProfileStringW(L"Effects", kSliders[i].key, b, g_ini.c_str());
    }
    for (int i = 0; i < kNumHotkeys; ++i) {
        swprintf(b, 64, L"0x%02X", g_hotkeys[i].vk);
        WritePrivateProfileStringW(L"Hotkeys", g_hotkeys[i].key, b, g_ini.c_str());
        std::wstring mk = std::wstring(g_hotkeys[i].key) + L"Mod";
        swprintf(b, 64, L"%u", g_hotkeys[i].mod);
        WritePrivateProfileStringW(L"Hotkeys", mk.c_str(), b, g_ini.c_str());
    }
    swprintf(b, 64, L"%d", g_monitorIndex);
    WritePrivateProfileStringW(L"General", L"Monitor", b, g_ini.c_str());
    WritePrivateProfileStringW(L"Experimental", L"StreamMode", g_streamMode ? L"1" : L"0", g_ini.c_str());
    if (g_petUnlocked) {
        WritePrivateProfileStringW(L"Pet", L"Unlocked", L"1", g_ini.c_str());
        WritePrivateProfileStringW(L"Pet", L"Enabled", g_petOn ? L"1" : L"0", g_ini.c_str());
    }
}

static void LoadIni() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_ini = path;
    size_t dot = g_ini.find_last_of(L'.');
    g_ini = (dot == std::wstring::npos ? g_ini : g_ini.substr(0, dot)) + L".ini";

    ResetParams();
    if (GetFileAttributesW(g_ini.c_str()) == INVALID_FILE_ATTRIBUTES) { SaveIni(); return; }

    wchar_t b[64];
    for (int i = 0; i < kNumSliders; ++i) {
        if (GetPrivateProfileStringW(L"Effects", kSliders[i].key, L"", b, 64, g_ini.c_str()) > 0) {
            float v = (float)wcstod(b, nullptr);
            g_p.*(kSliders[i].field) = std::min(kSliders[i].mx, std::max(kSliders[i].mn, v));
        }
    }
    for (int i = 0; i < kNumHotkeys; ++i) {
        if (GetPrivateProfileStringW(L"Hotkeys", g_hotkeys[i].key, L"", b, 64, g_ini.c_str()) > 0)
            g_hotkeys[i].vk = (UINT)wcstol(b, nullptr, 0);
        std::wstring mk = std::wstring(g_hotkeys[i].key) + L"Mod";
        if (GetPrivateProfileStringW(L"Hotkeys", mk.c_str(), L"", b, 64, g_ini.c_str()) > 0)
            g_hotkeys[i].mod = (UINT)wcstol(b, nullptr, 0) & (MOD_ALT | MOD_CONTROL | MOD_SHIFT);
    }
    g_monitorIndex = (int)GetPrivateProfileIntW(L"General", L"Monitor", 0, g_ini.c_str());
    g_streamMode  = GetPrivateProfileIntW(L"Experimental", L"StreamMode", 0, g_ini.c_str()) != 0;
    g_petUnlocked = GetPrivateProfileIntW(L"Pet", L"Unlocked", 0, g_ini.c_str()) != 0;
    g_petOn       = g_petUnlocked && GetPrivateProfileIntW(L"Pet", L"Enabled", 1, g_ini.c_str()) != 0;
    // Home/End/Insert are ReShade's usual menu keys. RegisterHotKey would swallow them, so never use them.
    for (int i = 0; i < kNumHotkeys; ++i)
        if (g_hotkeys[i].mod == 0 && (g_hotkeys[i].vk == VK_HOME || g_hotkeys[i].vk == VK_END || g_hotkeys[i].vk == VK_INSERT))
            g_hotkeys[i].vk = (i == 1) ? VK_F9 : VK_F11;
}

// ---------------------------------------------------------------- D3D / DXGI
static bool SelectOutput(int index) {
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g_factory)))) return false;
    ComPtr<IDXGIAdapter1> ad;
    int n = 0;
    for (UINT a = 0; g_factory->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; ad->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od;
            out->GetDesc(&od);
            if (od.AttachedToDesktop && n++ == index) {
                g_adapter = ad;
                out.As(&g_output);
                g_mon = od.DesktopCoordinates;
                return g_output != nullptr;
            }
        }
    }
    return false;
}

static HRESULT CreateDuplication() {
    g_dup.Reset();
    return g_output->DuplicateOutput(g_dev.Get(), &g_dup);
}

static void InitD3D() {
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(g_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) Fail(L"Could not create a Direct3D 11 device.", hr);

    int w = g_mon.right - g_mon.left, h = g_mon.bottom - g_mon.top;
    g_outW = w; g_outH = h; g_outRect = g_mon;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width  = w;
    sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count  = 1;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount       = 1;
    sd.OutputWindow      = g_overlay;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;
    hr = g_factory->CreateSwapChain(g_dev.Get(), &sd, &g_swap);
    if (FAILED(hr)) Fail(L"Could not create the swap chain.", hr);
    g_factory->MakeWindowAssociation(g_overlay, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    ComPtr<ID3D11Texture2D> bb;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);

    ComPtr<ID3DBlob> vsb, psb, err;
    hr = D3DCompile(kHLSL, strlen(kHLSL), "ShaderOverlay", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) Fail(L"Vertex shader compile failed.", hr);
    hr = D3DCompile(kHLSL, strlen(kHLSL), "ShaderOverlay", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        if (err) MessageBoxA(nullptr, (const char*)err->GetBufferPointer(), "Shader error", MB_ICONERROR);
        Fail(L"Pixel shader compile failed.", hr);
    }
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(Params);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&cbd, nullptr, &g_cb);

    D3D11_SAMPLER_DESC smd{};
    smd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.MaxLOD   = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smd, &g_smp);

    hr = CreateDuplication();
    if (FAILED(hr)) Fail(L"Desktop Duplication failed. Try running on the monitor your GPU drives, and close other capture tools that block it.", hr);
}

static void EnsureCaptureTex(UINT w, UINT h, DXGI_FORMAT fmt) {
    if (g_capTex) {
        D3D11_TEXTURE2D_DESC c;
        g_capTex->GetDesc(&c);
        if (c.Width == w && c.Height == h && c.Format == fmt) return;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.Format = fmt;
    d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_capTex.Reset(); g_capSrv.Reset();
    g_dev->CreateTexture2D(&d, nullptr, &g_capTex);
    g_dev->CreateShaderResourceView(g_capTex.Get(), nullptr, &g_capSrv);
    g_p.texelW = 1.0f / w;
    g_p.texelH = 1.0f / h;
}

static void EnsureCaptureTexture(ID3D11Texture2D* src) {
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    if (g_capTex) {
        D3D11_TEXTURE2D_DESC c;
        g_capTex->GetDesc(&c);
        if (c.Width == d.Width && c.Height == d.Height && c.Format == d.Format) return;
    }
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    d.MipLevels = 1;
    d.ArraySize = 1;
    g_capTex.Reset(); g_capSrv.Reset();
    g_dev->CreateTexture2D(&d, nullptr, &g_capTex);
    g_dev->CreateShaderResourceView(g_capTex.Get(), nullptr, &g_capSrv);
    g_p.texelW = 1.0f / d.Width;
    g_p.texelH = 1.0f / d.Height;
}

static void Draw() {
    if (!g_haveFrame) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &g_p, sizeof(Params));
        g_ctx->Unmap(g_cb.Get(), 0);
    }
    D3D11_VIEWPORT vp{ 0, 0, (float)g_outW, (float)g_outH, 0, 1 };
    ID3D11RenderTargetView* rtv = g_rtv.Get();
    ID3D11ShaderResourceView* srv = g_capSrv.Get();
    ID3D11SamplerState* smp = g_smp.Get();
    ID3D11Buffer* cb = g_cb.Get();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->PSSetSamplers(0, 1, &smp);
    g_ctx->PSSetConstantBuffers(0, 1, &cb);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSrv);
    g_swap->Present(0, 0);
    g_dirty = false;
}

static void FixZOrder();
// ---------------------------------------------------------------- stream mode (experimental)
// Normal mode hides the overlay from ALL capture (so Desktop Duplication doesn't feed our own output back in),
// which also hides it from Discord / OBS. Stream mode instead captures only the game WINDOW
// (Windows.Graphics.Capture, which never includes other windows), so the overlay can stay visible to capture.
static void ResizeOverlay(const RECT& r) {
    int w = std::max(16, (int)(r.right - r.left)), h = std::max(16, (int)(r.bottom - r.top));
    if (w == g_outW && h == g_outH && r.left == g_outRect.left && r.top == g_outRect.top) return;
    SetWindowPos(g_overlay, nullptr, r.left, r.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (w != g_outW || h != g_outH) {
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        g_rtv.Reset();
        g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        ComPtr<ID3D11Texture2D> bb;
        g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
        g_dev->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);
    }
    g_outW = w; g_outH = h; g_outRect = r;
    g_dirty = true;
}

static bool IsGoodTarget(HWND h) {
    if (!h || h == g_overlay || h == g_menu || !IsWindowVisible(h) || IsIconic(h)) return false;
    if (GetAncestor(h, GA_ROOT) != h) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcscmp(cls, L"Windows.UI.Core.CoreWindow") ||
        !wcscmp(cls, L"NotifyIconOverflowWindow") || !wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland")) return false;
    RECT r; GetWindowRect(h, &r);
    return (r.right - r.left) >= 200 && (r.bottom - r.top) >= 150;
}

static RECT TargetBounds(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
    return r;
}

#if SO_HAS_WGC
namespace WGC  = ABI::Windows::Graphics::Capture;
namespace WD3D = ABI::Windows::Graphics::DirectX::Direct3D11;
static ComPtr<WD3D::IDirect3DDevice>              g_wrtDev;
static ComPtr<WGC::IGraphicsCaptureItem>          g_item;
static ComPtr<WGC::IDirect3D11CaptureFramePool>   g_pool;
static ComPtr<WGC::IGraphicsCaptureSession>       g_session;
static ABI::Windows::Graphics::SizeInt32          g_poolSize{};

static void CapStop() {
    if (g_session) { ComPtr<ABI::Windows::Foundation::IClosable> c; if (SUCCEEDED(g_session.As(&c))) c->Close(); }
    if (g_pool)    { ComPtr<ABI::Windows::Foundation::IClosable> c; if (SUCCEEDED(g_pool.As(&c))) c->Close(); }
    g_session.Reset(); g_pool.Reset(); g_item.Reset();
}

static bool CapStart(HWND target) {
    using Microsoft::WRL::Wrappers::HStringReference;
    CapStop();
    static bool roInit = false;
    if (!roInit) { RoInitialize(RO_INIT_SINGLETHREADED); roInit = true; }
    if (!g_wrtDev) {
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(g_ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IInspectable> insp;
        if (FAILED(g_dev.As(&dxgi)) || FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), &insp)) || FAILED(insp.As(&g_wrtDev))) {
            g_streamInfo = L"Could not start Windows capture on this PC.";
            return false;
        }
    }
    ComPtr<IGraphicsCaptureItemInterop> interop;
    HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem).Get(), IID_PPV_ARGS(&interop));
    if (SUCCEEDED(hr)) hr = interop->CreateForWindow(target, IID_PPV_ARGS(&g_item));
    if (FAILED(hr)) { g_streamInfo = L"This window can't be captured (try running as admin if the game is)."; return false; }
    g_item->get_Size(&g_poolSize);
    ComPtr<WGC::IDirect3D11CaptureFramePoolStatics2> statics;
    hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool).Get(), IID_PPV_ARGS(&statics));
    if (SUCCEEDED(hr)) hr = statics->CreateFreeThreaded(g_wrtDev.Get(),
        ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, g_poolSize, &g_pool);
    if (SUCCEEDED(hr)) hr = g_pool->CreateCaptureSession(g_item.Get(), &g_session);
    if (FAILED(hr)) { CapStop(); g_streamInfo = L"Windows capture failed to start."; return false; }
    ComPtr<WGC::IGraphicsCaptureSession2> s2;
    if (SUCCEEDED(g_session.As(&s2))) s2->put_IsCursorCaptureEnabled(false);
    ComPtr<WGC::IGraphicsCaptureSession3> s3;          // Windows 11: no yellow capture border
    if (SUCCEEDED(g_session.As(&s3))) s3->put_IsBorderRequired(false);
    if (FAILED(g_session->StartCapture())) { CapStop(); g_streamInfo = L"Windows capture failed to start."; return false; }
    return true;
}

static bool CapGrab() {
    if (!g_pool) return false;
    ComPtr<WGC::IDirect3D11CaptureFrame> frame;
    if (FAILED(g_pool->TryGetNextFrame(&frame)) || !frame) return false;
    ABI::Windows::Graphics::SizeInt32 cs{};
    frame->get_ContentSize(&cs);
    ComPtr<WD3D::IDirect3DSurface> surf;
    frame->get_Surface(&surf);
    ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    ComPtr<ID3D11Texture2D> tex;
    bool ok = false;
    if (surf && SUCCEEDED(surf.As(&access)) && SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
        UINT w = std::min((UINT)std::max(1, cs.Width), d.Width), h = std::min((UINT)std::max(1, cs.Height), d.Height);
        EnsureCaptureTex(w, h, d.Format);
        D3D11_BOX box = { 0, 0, 0, w, h, 1 };
        g_ctx->CopySubresourceRegion(g_capTex.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
        g_haveFrame = ok = true;
    }
    ComPtr<ABI::Windows::Foundation::IClosable> c;
    if (SUCCEEDED(frame.As(&c))) c->Close();
    if (cs.Width > 0 && (cs.Width != g_poolSize.Width || cs.Height != g_poolSize.Height)) {   // window resized
        g_poolSize = cs;
        g_pool->Recreate(g_wrtDev.Get(), ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, cs);
    }
    return ok;
}
#else
// MinGW fallback: PrintWindow(PW_RENDERFULLCONTENT). Slower, and some games come out black.
static HDC     g_pwDC = nullptr;
static HBITMAP g_pwBmp = nullptr;
static void*   g_pwBits = nullptr;
static int     g_pwW = 0, g_pwH = 0;
static DWORD   g_pwLast = 0;
static void CapStop() {}
static bool CapStart(HWND) { return true; }
static bool CapGrab() {
    if (GetTickCount() - g_pwLast < 33) return false;          // ~30 fps cap
    g_pwLast = GetTickCount();
    RECT r; GetWindowRect(g_streamTarget, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;
    if (w != g_pwW || h != g_pwH) {
        if (g_pwBmp) DeleteObject(g_pwBmp);
        if (!g_pwDC) g_pwDC = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        g_pwBmp = CreateDIBSection(g_pwDC, &bi, DIB_RGB_COLORS, &g_pwBits, nullptr, 0);
        SelectObject(g_pwDC, g_pwBmp);
        g_pwW = w; g_pwH = h;
    }
    if (!PrintWindow(g_streamTarget, g_pwDC, 2 /*PW_RENDERFULLCONTENT*/)) return false;
    GdiFlush();
    EnsureCaptureTex(w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
    g_ctx->UpdateSubresource(g_capTex.Get(), 0, nullptr, g_pwBits, w * 4, 0);
    g_haveFrame = true;
    return true;
}
#endif

static std::wstring WindowTitle(HWND h) {
    wchar_t t[128] = {};
    GetWindowTextW(h, t, 128);
    return t[0] ? t : L"(untitled window)";
}

static void StreamRetarget(HWND h) {
    if (h == g_streamTarget) return;
    g_streamTarget = h;
    g_haveFrame = false;
    ShowWindow(g_overlay, SW_HIDE);
    if (CapStart(h)) g_streamInfo = L"Capturing: " + WindowTitle(h);
    ResizeOverlay(TargetBounds(h));
    g_uiDirty = true;
}

static void SetStreamMode(bool on) {
    g_streamMode = on;
    g_haveFrame = false;
    if (on) {
        SetWindowDisplayAffinity(g_overlay, WDA_NONE);         // Discord / OBS can see the overlay now
        g_streamTarget = nullptr;
        HWND fg = GetForegroundWindow();
        HWND pick = IsGoodTarget(fg) ? fg : (IsGoodTarget(g_prevFg) ? g_prevFg : nullptr);
        ShowWindow(g_overlay, SW_HIDE);
        if (pick) StreamRetarget(pick);
        else g_streamInfo = L"Waiting for a game window. Click into your game.";
    } else {
        CapStop();
        g_streamTarget = nullptr;
        g_streamInfo.clear();
        SetWindowDisplayAffinity(g_overlay, WDA_EXCLUDEFROMCAPTURE);
        ResizeOverlay(g_mon);
        g_dup.Reset();                                         // fresh desktop capture
        if (g_effectsOn) ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
        FixZOrder();
    }
    g_dirty = g_uiDirty = true;
}

// Called ~2x a second: follow alt-tab, window moves and minimize.
static void StreamHousekeeping() {
    if (!g_streamMode) return;
    HWND fg = GetForegroundWindow();
    if (IsGoodTarget(fg) && fg != g_streamTarget) StreamRetarget(fg);
    if (!g_streamTarget) return;
    if (!IsWindow(g_streamTarget)) { CapStop(); g_streamTarget = nullptr; g_streamInfo = L"Game closed. Waiting for a window."; ShowWindow(g_overlay, SW_HIDE); g_uiDirty = true; return; }
    bool visible = !IsIconic(g_streamTarget) && IsWindowVisible(g_streamTarget);
    if (g_effectsOn) ShowWindow(g_overlay, (visible && g_haveFrame) ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible) ResizeOverlay(TargetBounds(g_streamTarget));
}

static void RenderFrame() {
    if (g_streamMode) {
        if (g_streamTarget && CapGrab()) {
            Draw();
            if (g_effectsOn && !IsWindowVisible(g_overlay) && !IsIconic(g_streamTarget)) {
                ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
                FixZOrder();
            }
        } else {
            if (g_haveFrame && (g_dirty || g_interactive)) Draw();
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 3, QS_ALLINPUT);
        }
        return;
    }
    if (!g_dup) {
        if (FAILED(CreateDuplication())) { Sleep(100); return; }
    }
    DXGI_OUTDUPL_FRAME_INFO fi;
    ComPtr<IDXGIResource> res;
    HRESULT hr = g_dup->AcquireNextFrame(16, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) { if (g_haveFrame && (g_dirty || g_interactive)) Draw(); return; }
    if (FAILED(hr)) {           // ACCESS_LOST (mode change, UAC, fullscreen switch...)
        g_dup.Reset();
        Sleep(50);
        return;
    }
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(res.As(&tex))) {
        EnsureCaptureTexture(tex.Get());
        g_ctx->CopyResource(g_capTex.Get(), tex.Get());
        g_haveFrame = true;
    }
    g_dup->ReleaseFrame();
    Draw();
}

// ---------------------------------------------------------------- window management
static void FixZOrder() {
    const UINT f = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    if (IsWindowVisible(g_menu)) {
        SetWindowPos(g_menu, HWND_TOPMOST, 0, 0, 0, 0, f);
        if (g_effectsOn) SetWindowPos(g_overlay, g_menu, 0, 0, 0, 0, f);   // effects just below the menu
    } else if (g_effectsOn) {
        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, f);
    }
}

static void ForceForeground(HWND h) {
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    SetFocus(h);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
}

static void RegisterAll(bool showBox) {
    for (int i = 0; i < kNumHotkeys; ++i) UnregisterHotKey(g_menu, g_hotkeys[i].id);
    std::wstring failed;
    for (int i = 0; i < kNumHotkeys; ++i)
        if (!RegisterHotKey(g_menu, g_hotkeys[i].id, g_hotkeys[i].mod | MOD_NOREPEAT, g_hotkeys[i].vk))
            failed += VkName(g_hotkeys[i].mod, g_hotkeys[i].vk) + L"  ";
    if (!failed.empty()) {
        g_status = failed + L"is already used by another app. Pick a different key.";
        if (showBox) MessageBoxW(nullptr, g_status.c_str(), L"ShaderOverlay", MB_ICONWARNING);
    }
    g_uiDirty = true;
}

static void SetInteractive(bool on);
static void SetEffects(bool on) {
    if (!on && g_interactive) SetInteractive(false);
    g_effectsOn = on;
    bool show = on && (!g_streamMode || (g_streamTarget && g_haveFrame && !IsIconic(g_streamTarget)));
    ShowWindow(g_overlay, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (on) g_dirty = true;
    FixZOrder();
    g_uiDirty = true;
}

static void PaintMenu();

static void ShowMenu() {
    if (g_interactive) SetInteractive(false);
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_menu && fg != g_overlay) g_prevFg = fg;
    ClipCursor(nullptr);                 // games often lock the cursor
    g_uiDirty = true;
    PaintMenu();
    ShowWindow(g_menu, SW_SHOW);
    FixZOrder();
    ForceForeground(g_menu);
}

static void HideMenu() {
    if (g_capturing >= 0) { g_capturing = -1; RegisterAll(false); }
    if (g_drag >= 0) { g_drag = -1; ReleaseCapture(); }
    ShowWindow(g_menu, SW_HIDE);
    if (g_prevFg && IsWindow(g_prevFg)) ForceForeground(g_prevFg);   // give focus back to the game
}

static void ToggleMenu() { if (IsWindowVisible(g_menu)) HideMenu(); else ShowMenu(); }

// ReShade input mode: make the effect window a normal focusable window so ReShade
// (installed on ShaderOverlay.exe) receives keyboard + mouse and can show its menu.
static void SetInteractive(bool on) {
    if (on == g_interactive) return;
    if (on) {
        if (IsWindowVisible(g_menu)) HideMenu();
        if (!g_effectsOn) SetEffects(true);
        HWND fg = GetForegroundWindow();
        if (fg && fg != g_menu && fg != g_overlay) g_prevFg = fg;
    }
    g_interactive = on;
    LONG_PTR ex = GetWindowLongPtrW(g_overlay, GWL_EXSTYLE);
    if (on) ex &= ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    else    ex |=  (LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(g_overlay, GWL_EXSTYLE, ex);
    SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | (on ? 0 : SWP_NOACTIVATE));
    if (on) {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
        ForceForeground(g_overlay);
    } else if (g_prevFg && IsWindow(g_prevFg)) {
        ForceForeground(g_prevFg);
    }
    g_dirty = g_uiDirty = true;
}

// ---------------------------------------------------------------- drawing helpers
static void RR(GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = std::min(r * 2, std::min(w, h));
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}
static void FillRR(Graphics& g, float x, float y, float w, float h, float r, DWORD col) {
    GraphicsPath p; RR(p, x, y, w, h, r);
    SolidBrush b{ Color((ARGB)col) };
    g.FillPath(&b, &p);
}
static void StrokeRR(Graphics& g, float x, float y, float w, float h, float r, DWORD col) {
    GraphicsPath p; RR(p, x, y, w, h, r);
    Pen pen(Color((ARGB)col), std::max(1.0f, S(1)));
    g.DrawPath(&pen, &p);
}
static void Circle(Graphics& g, float cx, float cy, float r, DWORD col) {
    SolidBrush b{ Color((ARGB)col) };
    g.FillEllipse(&b, cx - r, cy - r, r * 2, r * 2);
}
static void Text(Graphics& g, const std::wstring& s, Font* f, DWORD col,
                 float x, float y, float w, float h, int align = 0, bool wrap = false) {
    StringFormat sf;
    sf.SetAlignment(align == 0 ? StringAlignmentNear : align == 1 ? StringAlignmentCenter : StringAlignmentFar);
    sf.SetLineAlignment(wrap ? StringAlignmentNear : StringAlignmentCenter);
    if (!wrap) { sf.SetFormatFlags(StringFormatFlagsNoWrap); sf.SetTrimming(StringTrimmingEllipsisCharacter); }
    SolidBrush b{ Color((ARGB)col) };
    g.DrawString(s.c_str(), -1, f, RectF(x, y, w, h), &sf, &b);
}
static void AddHit(float x, float y, float w, float h, int type, int idx = 0) {
    Hit t = { x, y, w, h, type, idx };
    g_hits.push_back(t);
}
static bool Hov(int type, int idx = 0) { return g_hoverType == type && g_hoverIdx == idx; }

static void Switch(Graphics& g, float x, float y, bool on) {
    FillRR(g, x, y, S(38), S(22), S(11), on ? 0xFF3B82F6 : 0xFF3A3A3A);
    Circle(g, on ? x + S(27) : x + S(11), y + S(11), S(8), 0xFFFFFFFF);
}
static void Pill(Graphics& g, float x, float y, float w, float h, const wchar_t* t, bool primary, bool hover) {
    if (primary) {
        FillRR(g, x, y, w, h, S(8), hover ? 0xFFFFFFFF : 0xFFE8E8E8);
        Text(g, t, fBold, 0xFF111111, x, y, w, h, 1);
    } else {
        FillRR(g, x, y, w, h, S(8), hover ? 0xFF303030 : 0xFF232323);
        StrokeRR(g, x, y, w, h, S(8), 0xFF3A3A3A);
        Text(g, t, fBold, 0xFFE5E5E5, x, y, w, h, 1);
    }
}

// Colors (Codex-style dark)
static const DWORD C_BACKDROP = 0x70000000, C_MAIN = 0xF7181818, C_SIDE = 0xF7222222, C_BORDER = 0xFF303030,
                   C_CARD = 0xFF1F1F1F, C_CARDBR = 0xFF2C2C2C, C_TEXT = 0xFFEDEDED, C_DIM = 0xFF8C8C8C,
                   C_HOVER = 0xFF2A2A2A, C_SEL = 0xFF323232;

// ---------------------------------------------------------------- Clawd, the tiny crab (easter egg)
static const char* kCrab[2][8] = {
    { ".OO......OO.",
      ".O........O.",
      ".OOOOOOOOOO.",
      "OO.OEOOEO.OO",
      ".OOOOOOOOOO.",
      "..OOOOOOOO..",
      ".O.O....O.O.",
      "O.O......O.O" },
    { ".OO......OO.",
      ".O........O.",
      ".OOOOOOOOOO.",
      "OO.OEOOEO.OO",
      ".OOOOOOOOOO.",
      "..OOOOOOOO..",
      "..O.O..O.O..",
      ".O.O....O.O." },
};
static const char* kHeart[5] = { ".X.X.", "XXXXX", "XXXXX", ".XXX.", "..X.." };
static const int kCrabW = 12, kCrabH = 8;

static void DrawCrab(Graphics& g, float x, float y, float px) {
    SolidBrush body{ Color(0xFFD97757) }, eye{ Color(0xFF1A1A1A) }, shade{ Color(0x55000000) };
    g.SetSmoothingMode(SmoothingModeNone);
    g.FillEllipse(&shade, x + px, y + kCrabH * px + (g_petJump ? px * 2 : 0), (kCrabW - 2) * px, px * 1.5f);
    float jy = g_petJump ? -px * (float)(4 - std::abs(g_petJump - 4)) * 1.5f : 0;
    for (int r = 0; r < kCrabH; ++r)
        for (int c = 0; c < kCrabW; ++c) {
            char ch = kCrab[g_petFrame & 1][r][g_petDir < 0 ? kCrabW - 1 - c : c];
            if (ch == 'E' && g_petBlink) ch = 'O';
            if (ch == 'O') g.FillRectangle(&body, x + c * px, y + r * px + jy, px, px);
            else if (ch == 'E') g.FillRectangle(&eye, x + c * px, y + r * px + jy, px, px);
        }
    if (g_petHeart > 0) {
        SolidBrush heart{ Color((BYTE)std::min(255, g_petHeart * 25), 0xF4, 0x72, 0xB6) };
        float hx = x + (kCrabW / 2 - 2.5f) * px, hy = y - px * 7 - (12 - g_petHeart) * px * 0.4f;
        for (int r = 0; r < 5; ++r) for (int c = 0; c < 5; ++c)
            if (kHeart[r][c] == 'X') g.FillRectangle(&heart, hx + c * px, hy + r * px, px, px);
    }
    g.SetSmoothingMode(SmoothingModeAntiAlias);
}

static void PetTick(float minX, float maxX) {
    g_petX += g_petDir * S(2);
    if (g_petX < minX) { g_petX = minX; g_petDir = 1; }
    if (g_petX > maxX) { g_petX = maxX; g_petDir = -1; }
    g_petFrame++;
    if (g_petJump > 0) g_petJump--;
    if (g_petHeart > 0) g_petHeart--;
    g_petBlink = (rand() % 30 == 0) ? 2 : std::max(0, g_petBlink - 1);
    if (rand() % 80 == 0) g_petDir = -g_petDir;
}
static float g_petMin = 0, g_petMax = 0;

static void UnlockPet() {
    g_petUnlocked = g_petOn = true;
    g_petJump = 8; g_petHeart = 12;
    g_status = L"You found Clawd! Toggle him in Labs.";
    SaveIni();
}

// ---------------------------------------------------------------- menu painting
static void PaintMenu() {
    const int W = g_mon.right - g_mon.left, H = g_mon.bottom - g_mon.top;
    g_hits.clear();
    {
        Graphics g(g_bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color((ARGB)C_BACKDROP));

        const float PW = S(900), PH = S(600), SB = S(232), HDR = S(52);
        const float px = (float)(int)((W - PW) / 2), py = (float)(int)((H - PH) / 2);

        // soft shadow
        for (int i = 6; i >= 1; --i)
            FillRR(g, px - S(i * 2.0f), py - S(i * 2.0f) + S(8), PW + S(i * 4.0f), PH + S(i * 4.0f), S(12 + i * 2.0f), 0x14000000);

        // panel + sidebar
        GraphicsPath pp; RR(pp, px, py, PW, PH, S(12));
        { SolidBrush b{ Color((ARGB)C_MAIN) }; g.FillPath(&b, &pp); }
        g.SetClip(&pp);
        { SolidBrush b{ Color((ARGB)C_SIDE) }; g.FillRectangle(&b, px, py, SB, PH); }
        { Pen pen(Color((ARGB)C_BORDER), std::max(1.0f, S(1))); g.DrawLine(&pen, px + SB, py, px + SB, py + PH); }
        g.ResetClip();
        StrokeRR(g, px, py, PW, PH, S(12), C_BORDER);
        AddHit(px, py, PW, PH, H_PANEL);

        // window dots (red closes)
        const DWORD dots[3] = { 0xFFFF5F57, 0xFFFEBC2E, 0xFF28C840 };
        for (int i = 0; i < 3; ++i) Circle(g, px + S(22 + i * 20.0f), py + S(22), S(6), dots[i]);
        if (Hov(H_CLOSE)) Text(g, L"\u00D7", fBold, 0xFF4A0000, px + S(16), py + S(15), S(12), S(14), 1);
        AddHit(px + S(14), py + S(14), S(16), S(16), H_CLOSE);

        // version pill (the Codex "Update" button nod)
        FillRR(g, px + SB - S(62), py + S(12), S(48), S(20), S(10), Hov(H_VERSION) ? 0xFF4280FF : 0xFF2F6FED);
        Text(g, L"v1.2", fSmall, 0xFFFFFFFF, px + SB - S(62), py + S(12), S(48), S(20), 1);
        AddHit(px + SB - S(62), py + S(12), S(48), S(20), H_VERSION);

        // nav
        const wchar_t* navIcon[4] = { L"\uE790", L"\uE765", L"\uE734", L"\uE946" };
        const wchar_t* navText[4] = { L"Effects", L"Hotkeys", L"Labs", L"About" };
        float ny = py + S(50);
        for (int i = 0; i < 4; ++i) {
            float x = px + S(10), y = ny + i * S(34), w = SB - S(20), h = S(30);
            if (g_page == i) FillRR(g, x, y, w, h, S(7), C_SEL);
            else if (Hov(H_NAV, i)) FillRR(g, x, y, w, h, S(7), C_HOVER);
            Text(g, navIcon[i], fIcon, g_page == i ? C_TEXT : 0xFFB5B5B5, x + S(10), y, S(20), h, 1);
            Text(g, navText[i], fBody, g_page == i ? C_TEXT : 0xFFC8C8C8, x + S(38), y, w - S(40), h);
            AddHit(x, y, w, h, H_NAV, i);
        }

        // status section
        float sy = ny + S(4 * 34 + 22);
        Text(g, L"Status", fSmall, C_DIM, px + S(20), sy, SB - S(40), S(20));
        sy += S(26);
        Circle(g, px + S(26), sy + S(10), S(4), g_effectsOn ? 0xFF22C55E : 0xFF6B6B6B);
        Text(g, g_interactive ? L"ReShade input mode" : g_effectsOn ? L"Effects running" : L"Effects paused", fBody, 0xFFC8C8C8, px + S(38), sy, SB - S(50), S(20));
        sy += S(26);
        wchar_t res[64];
        swprintf(res, 64, L"Monitor %d  \u00B7  %d\u00D7%d", g_monitorIndex + 1, W, H);
        Text(g, res, fBody, 0xFFC8C8C8, px + S(38), sy, SB - S(50), S(20));
        if (g_streamMode) {
            sy += S(26);
            Circle(g, px + S(26), sy + S(10), S(4), 0xFFA855F7);
            Text(g, L"Visible to screen share", fBody, 0xFFC8C8C8, px + S(38), sy, SB - S(50), S(20));
        }

        // sidebar footer
        Text(g, L"\uE7E8", fIcon, C_DIM, px + S(16), py + PH - S(40), S(20), S(26), 1);
        Text(g, VkName(g_hotkeys[2].mod, g_hotkeys[2].vk) + L" to quit", fSmall, C_DIM, px + S(42), py + PH - S(40), SB - S(52), S(26));

        // header
        const float mx = px + SB, mw = PW - SB;
        const wchar_t* titles[4] = { L"Effects", L"Hotkeys", L"Labs", L"About" };
        Text(g, titles[g_page], fTitle, C_TEXT, mx + S(22), py, S(200), HDR);
        float bx = px + PW - S(20) - S(72);
        Pill(g, bx, py + S(12), S(72), S(28), L"Save", true, Hov(H_SAVE));
        AddHit(bx, py + S(12), S(72), S(28), H_SAVE);
        if (g_page == 0) {
            bx -= S(82);
            Pill(g, bx, py + S(12), S(72), S(28), L"Reset", false, Hov(H_RESET));
            AddHit(bx, py + S(12), S(72), S(28), H_RESET);
        }

        // content column
        const float cw = std::min(mw - S(80), S(560)), cx = mx + (mw - cw) / 2;
        float cy = py + HDR + S(14);

        if (g_page == 0) {
            FillRR(g, cx, cy, cw, S(60), S(12), Hov(H_TOGGLE) ? 0xFF242424 : C_CARD);
            StrokeRR(g, cx, cy, cw, S(60), S(12), C_CARDBR);
            Text(g, L"Post-processing", fBold, C_TEXT, cx + S(18), cy + S(10), cw - S(100), S(20));
            Text(g, L"Changes apply live. Double-click a slider to reset it.", fSmall, C_DIM, cx + S(18), cy + S(30), cw - S(100), S(20));
            Switch(g, cx + cw - S(56), cy + S(19), g_effectsOn);
            AddHit(cx, cy, cw, S(60), H_TOGGLE);
            cy += S(60 + 18);

            Text(g, L"Adjustments", fSmall, C_DIM, cx + S(4), cy, cw, S(18));
            cy += S(24);
            const float rowH = S(44), cardH = S(12) + rowH * kNumSliders;
            FillRR(g, cx, cy, cw, cardH, S(12), C_CARD);
            StrokeRR(g, cx, cy, cw, cardH, S(12), C_CARDBR);
            g_tx0 = cx + S(160);
            g_tx1 = cx + cw - S(80);
            for (int i = 0; i < kNumSliders; ++i) {
                const SliderDef& d = kSliders[i];
                float rx = cx + S(6), ry = cy + S(6) + i * rowH, rw = cw - S(12);
                bool active = g_drag == i || (g_drag < 0 && Hov(H_SLIDER, i));
                if (active) FillRR(g, rx, ry, rw, rowH, S(8), 0xFF262626);
                Text(g, d.label, fBody, C_TEXT, rx + S(14), ry, S(130), rowH);

                float v = g_p.*(d.field), n = (v - d.mn) / (d.mx - d.mn);
                float ty = ry + rowH / 2, kx = g_tx0 + n * (g_tx1 - g_tx0);
                FillRR(g, g_tx0, ty - S(2), g_tx1 - g_tx0, S(4), S(2), 0xFF3A3A3A);
                if (kx > g_tx0) FillRR(g, g_tx0, ty - S(2), kx - g_tx0, S(4), S(2), 0xFFD9D9D9);
                Circle(g, kx, ty, active ? S(8) : S(7), 0xFFFFFFFF);

                wchar_t vb[32];
                swprintf(vb, 32, L"%.2f", v);
                Text(g, vb, fBody, active ? C_TEXT : 0xFFB0B0B0, g_tx1 + S(10), ry, cx + cw - S(20) - (g_tx1 + S(10)), rowH, 2);
                AddHit(g_tx0 - S(10), ry, g_tx1 - g_tx0 + S(20), rowH, H_SLIDER, i);
            }
        } else if (g_page == 1) {
            Text(g, L"Keyboard shortcuts", fSmall, C_DIM, cx + S(4), cy, cw, S(18));
            cy += S(24);
            const float rowH = S(56), cardH = rowH * kNumHotkeys;
            FillRR(g, cx, cy, cw, cardH, S(12), C_CARD);
            StrokeRR(g, cx, cy, cw, cardH, S(12), C_CARDBR);
            for (int i = 0; i < kNumHotkeys; ++i) {
                float ry = cy + i * rowH;
                if (i > 0) { Pen pen(Color((ARGB)C_CARDBR), std::max(1.0f, S(1))); g.DrawLine(&pen, cx + S(16), ry, cx + cw - S(16), ry); }
                Text(g, g_hotkeys[i].label, fBody, C_TEXT, cx + S(18), ry, cw - S(220), rowH);
                float chW = S(170), chH = S(30), chX = cx + cw - S(18) - chW, chY = ry + (rowH - chH) / 2;
                bool cap = g_capturing == i;
                FillRR(g, chX, chY, chW, chH, S(8), cap ? 0xFF1E2A44 : (Hov(H_HOTKEY, i) ? 0xFF303030 : 0xFF272727));
                StrokeRR(g, chX, chY, chW, chH, S(8), cap ? 0xFF3B82F6 : 0xFF3A3A3A);
                Text(g, cap ? std::wstring(L"Press a key\u2026") : VkName(g_hotkeys[i].mod, g_hotkeys[i].vk),
                     fBold, cap ? 0xFF93B4FF : C_TEXT, chX, chY, chW, chH, 1);
                AddHit(chX, chY, chW, chH, H_HOTKEY, i);
            }
            cy += cardH + S(16);
            Text(g, L"Click a shortcut, then press the new key (Ctrl / Alt / Shift combos work). Esc cancels. Hit Save to keep it.",
                 fSmall, C_DIM, cx + S(4), cy, cw - S(8), S(40), 0, true);
            if (!g_status.empty())
                Text(g, g_status, fSmall, 0xFFF59E0B, cx + S(4), cy + S(44), cw - S(8), S(40), 0, true);
        } else if (g_page == 2) {
            Text(g, L"Experimental", fSmall, C_DIM, cx + S(4), cy, cw, S(18));
            cy += S(24);
            const float cardH = S(128);
            FillRR(g, cx, cy, cw, cardH, S(12), Hov(H_STREAM) ? 0xFF242424 : C_CARD);
            StrokeRR(g, cx, cy, cw, cardH, S(12), C_CARDBR);
            Text(g, L"Show effects in screen share", fBold, C_TEXT, cx + S(18), cy + S(12), cw - S(160), S(20));
            FillRR(g, cx + S(212), cy + S(13), S(40), S(18), S(9), 0xFF3B2A55);
            Text(g, L"BETA", fSmall, 0xFFD8B4FE, cx + S(212), cy + S(13), S(40), S(18), 1);
            Text(g, L"Discord / OBS full-screen share will see your effects and ReShade. Captures the focused game window "
                    L"instead of the screen. Borderless / windowed games only, costs some FPS, and the overlay covers "
                    L"anything on top of the game.",
                 fSmall, C_DIM, cx + S(18), cy + S(36), cw - S(100), S(60), 0, true);
            Switch(g, cx + cw - S(56), cy + S(16), g_streamMode);
            if (g_streamMode && !g_streamInfo.empty())
                Text(g, g_streamInfo, fSmall, 0xFFA78BFA, cx + S(18), cy + cardH - S(28), cw - S(36), S(20));
            AddHit(cx, cy, cw, cardH, H_STREAM);
            cy += cardH + S(16);
            if (!SO_HAS_WGC) {
                Text(g, L"This build uses the slower PrintWindow capture (built with MinGW). Build with GitHub / Visual Studio for the fast one.",
                     fSmall, 0xFFF59E0B, cx + S(4), cy, cw - S(8), S(36), 0, true);
                cy += S(44);
            }
            if (g_petUnlocked) {
                FillRR(g, cx, cy, cw, S(60), S(12), Hov(H_PET) ? 0xFF242424 : C_CARD);
                StrokeRR(g, cx, cy, cw, S(60), S(12), C_CARDBR);
                Text(g, L"Clawd", fBold, C_TEXT, cx + S(18), cy + S(10), cw - S(100), S(20));
                Text(g, L"A tiny crab who lives in this menu. Click him.", fSmall, C_DIM, cx + S(18), cy + S(30), cw - S(100), S(20));
                Switch(g, cx + cw - S(56), cy + S(19), g_petOn);
                AddHit(cx, cy, cw, S(60), H_PET);
            }
        } else {
            FillRR(g, cx, cy, cw, S(300), S(12), C_CARD);
            StrokeRR(g, cx, cy, cw, S(300), S(12), C_CARDBR);
            Text(g, L"ShaderOverlay 1.2", fTitle, C_TEXT, cx + S(20), cy + S(16), cw - S(40), S(26));
            Text(g,
                 L"Captures your screen, runs the effects on your GPU (Direct3D 11) and draws the result in a click-through "
                 L"window over the game. Nothing is injected into the game process.\n\n"
                 L"\u2022  Run games in Borderless or Windowed mode.\n"
                 L"\u2022  Using ReShade on ShaderOverlay.exe? Press F10 first, then ReShade's key. F10 again goes back to the game.\n"
                 L"\u2022  If the game runs as administrator, run ShaderOverlay as administrator too.\n"
                 L"\u2022  The tray icon also opens this menu (left-click) or quits (right-click).\n"
                 L"\u2022  Click outside this panel or press Esc to close the menu.\n"
                 L"\u2022  Settings live in ShaderOverlay.ini next to the exe.",
                 fBody, 0xFFC8C8C8, cx + S(20), cy + S(52), cw - S(40), S(240), 0, true);
        }

        // pet
        g_petMin = mx + S(16);
        g_petMax = px + PW - S(16) - kCrabW * S(3);
        if (g_petOn) {
            if (g_petX < g_petMin || g_petX > g_petMax) g_petX = g_petMin;
            float cyb = py + PH - S(40) - kCrabH * S(3);
            DrawCrab(g, g_petX, cyb, S(3));
            AddHit(g_petX, cyb - S(10), kCrabW * S(3), kCrabH * S(3) + S(10), H_CRAB);
        }
        if (g_page != 1 && !g_status.empty() && g_status.find(L"Clawd") != std::wstring::npos)
            Text(g, g_status, fSmall, 0xFFD97757, mx, py + PH - S(62) - kCrabH * S(3), mw, S(20), 1);

        // footer hint
        std::wstring hint = VkName(g_hotkeys[0].mod, g_hotkeys[0].vk) + L"  effects    \u00B7    " +
                            VkName(g_hotkeys[1].mod, g_hotkeys[1].vk) + L"  menu    \u00B7    Esc  close";
        Text(g, hint, fSmall, 0xFF6E6E6E, mx, py + PH - S(36), mw, S(24), 1);
    }

    POINT pos = { g_mon.left, g_mon.top }, src = { 0, 0 };
    SIZE  sz = { W, H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_menu, nullptr, &pos, &sz, g_memDC, &src, 0, &bf, ULW_ALPHA);
    g_uiDirty = false;
}

// ---------------------------------------------------------------- menu input
static int HitTest(float x, float y) {
    for (int i = (int)g_hits.size() - 1; i >= 0; --i) {
        const Hit& h = g_hits[i];
        if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) return i;
    }
    return -1;
}

static void SliderFromX(float x) {
    if (g_drag < 0) return;
    const SliderDef& d = kSliders[g_drag];
    float n = std::min(1.0f, std::max(0.0f, (x - g_tx0) / (g_tx1 - g_tx0)));
    g_p.*(d.field) = d.mn + n * (d.mx - d.mn);
    g_dirty = g_uiDirty = true;
}

static void StartCapture(int i) {
    g_capturing = i;
    g_status.clear();
    for (int k = 0; k < kNumHotkeys; ++k) UnregisterHotKey(g_menu, g_hotkeys[k].id);   // let keys reach us
    g_uiDirty = true;
}

static void OnClick(HWND h, float x, float y, bool dbl) {
    int hi = HitTest(x, y);
    if (hi < 0) { HideMenu(); return; }                 // clicked the backdrop
    Hit t = g_hits[hi];
    if (g_capturing >= 0 && t.type != H_HOTKEY) { g_capturing = -1; RegisterAll(false); }
    switch (t.type) {
    case H_CLOSE:  HideMenu(); return;
    case H_NAV:    g_page = t.idx; break;
    case H_TOGGLE: SetEffects(!g_effectsOn); break;
    case H_RESET:  ResetParams(); break;
    case H_SAVE:   SaveIni(); if (g_page == 1) g_status = L"Saved."; break;
    case H_STREAM: SetStreamMode(!g_streamMode); break;
    case H_PET:    g_petOn = !g_petOn; break;
    case H_CRAB:   g_petJump = 8; g_petHeart = 12; break;
    case H_VERSION: {
        DWORD now = GetTickCount();
        g_versionClicks = (now - g_versionClickT < 1500) ? g_versionClicks + 1 : 1;
        g_versionClickT = now;
        if (g_versionClicks >= 5 && !g_petUnlocked) UnlockPet();
        else if (g_versionClicks >= 5) { g_petOn = true; g_petJump = 8; g_petHeart = 12; }
        break;
    }
    case H_HOTKEY: if (g_capturing == t.idx) { g_capturing = -1; RegisterAll(false); } else StartCapture(t.idx); break;
    case H_SLIDER:
        if (dbl) {
            g_p.*(kSliders[t.idx].field) = kSliders[t.idx].def;
            g_dirty = true;
        } else {
            g_drag = t.idx;
            SetCapture(h);
            SliderFromX(x);
        }
        break;
    }
    g_uiDirty = true;
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open menu");
    AppendMenuW(m, MF_STRING | (g_effectsOn ? MF_CHECKED : 0), 2, L"Effects enabled");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"Quit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_menu);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_menu, nullptr);
    DestroyMenu(m);
    if (cmd == 1) ShowMenu();
    else if (cmd == 2) SetEffects(!g_effectsOn);
    else if (cmd == 3) g_running = false;
}

static LRESULT CALLBACK MenuProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        switch ((int)wp) {
        case 1: SetEffects(!g_effectsOn); break;
        case 2: ToggleMenu(); break;
        case 3: g_running = false; break;
        case 4: SetInteractive(!g_interactive); break;
        }
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ToggleMenu();
        else if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
        return 0;
    case WM_LBUTTONDOWN:
        OnClick(h, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp), false);
        return 0;
    case WM_LBUTTONDBLCLK:
        OnClick(h, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp), true);
        return 0;
    case WM_LBUTTONUP:
        if (g_drag >= 0) { g_drag = -1; ReleaseCapture(); g_uiDirty = true; }
        return 0;
    case WM_MOUSEMOVE: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        if (g_drag >= 0) SliderFromX(x);
        int hi = HitTest(x, y);
        int ht = hi >= 0 ? g_hits[hi].type : 0, hx = hi >= 0 ? g_hits[hi].idx : -1;
        if (ht != g_hoverType || hx != g_hoverIdx) { g_hoverType = ht; g_hoverIdx = hx; g_uiDirty = true; }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &p);
        int hi = HitTest((float)p.x, (float)p.y);
        if (hi >= 0 && g_hits[hi].type == H_SLIDER) {
            const SliderDef& d = kSliders[g_hits[hi].idx];
            float step = (d.mx - d.mn) / 100.0f * (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.0f : -1.0f);
            g_p.*(d.field) = std::min(d.mx, std::max(d.mn, g_p.*(d.field) + step));
            g_dirty = g_uiDirty = true;
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            bool hand = g_hoverType > H_PANEL;
            SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)wp;
        if (g_capturing >= 0) {
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return 0;
            if (vk != VK_ESCAPE) {
                UINT mod = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) |
                           (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
                           (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0);
                g_hotkeys[g_capturing].vk = vk;
                g_hotkeys[g_capturing].mod = mod;
            }
            g_capturing = -1;
            RegisterAll(false);
            return 0;
        }
        if (vk == VK_ESCAPE) { HideMenu(); return 0; }
        if (vk >= 'A' && vk <= 'Z') {
            g_typed += (wchar_t)vk;
            if (g_typed.size() > 8) g_typed.erase(0, g_typed.size() - 8);
            if (g_typed.size() >= 4 && (g_typed.compare(g_typed.size() - 4, 4, L"CRAB") == 0 ||
                                        g_typed.compare(g_typed.size() - 4, 4, L"CLAW") == 0)) {
                if (!g_petUnlocked) UnlockPet(); else { g_petOn = true; g_petJump = 8; g_petHeart = 12; }
                g_uiDirty = true;
            }
        }
        break;
    }
    case WM_CLOSE:
        HideMenu();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_interactive) {
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    } else if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    }
    if (msg == WM_CLOSE) return 0;
    return DefWindowProcW(h, msg, wp, lp);
}

static Font* MakeFont(const wchar_t* fam, const wchar_t* fallback, float px, int style) {
    Font* f = new Font(fam, px, style, UnitPixel);
    if (f->GetLastStatus() != Ok) { delete f; f = new Font(fallback, px, style, UnitPixel); }
    return f;
}

static void CreateWindows(HINSTANCE hi) {
    const int W = g_mon.right - g_mon.left, H = g_mon.bottom - g_mon.top;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = OverlayProc;
    wc.hInstance     = hi;
    wc.lpszClassName = L"ShaderOverlayWnd";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"ShaderOverlayWnd", L"ShaderOverlay", WS_POPUP,
        g_mon.left, g_mon.top, W, H, nullptr, nullptr, hi, nullptr);
    if (!g_overlay) Fail(L"Could not create overlay window.", HRESULT_FROM_WIN32(GetLastError()));
    SetLayeredWindowAttributes(g_overlay, 0, 255, LWA_ALPHA);
    if (!SetWindowDisplayAffinity(g_overlay, WDA_EXCLUDEFROMCAPTURE))
        Fail(L"Windows 10 version 2004 or newer is required (window capture exclusion is not supported).",
             HRESULT_FROM_WIN32(GetLastError()));

    WNDCLASSEXW mc = { sizeof(mc) };
    mc.style         = CS_DBLCLKS;
    mc.lpfnWndProc   = MenuProc;
    mc.hInstance     = hi;
    mc.lpszClassName = L"ShaderOverlayMenu";
    mc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&mc);

    // Full-desktop, per-pixel-alpha, always-on-top menu window
    g_menu = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"ShaderOverlayMenu", L"ShaderOverlay Menu", WS_POPUP,
        g_mon.left, g_mon.top, W, H, nullptr, nullptr, hi, nullptr);
    if (!g_menu) Fail(L"Could not create menu window.", HRESULT_FROM_WIN32(GetLastError()));
    SetWindowDisplayAffinity(g_menu, WDA_EXCLUDEFROMCAPTURE);   // never bake the menu into the effect pass

    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    if (auto fn = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"))
        g_s = fn(g_menu) / 96.0f;
    if (g_s < 1.0f) g_s = 1.0f;
    // make sure the panel fits on small screens
    { float fit = std::min(W / 960.0f, H / 660.0f); if (g_s > fit) g_s = std::max(0.75f, fit); }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_memDC = CreateCompatibleDC(nullptr);
    g_dib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
    if (!g_dib) Fail(L"Could not allocate the menu surface.");
    SelectObject(g_memDC, g_dib);
    g_bmp = new Bitmap(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)g_bits);

    fTitle = MakeFont(L"Segoe UI Semibold", L"Segoe UI", S(15), FontStyleRegular);
    fBody  = MakeFont(L"Segoe UI",          L"Tahoma",   S(13), FontStyleRegular);
    fBold  = MakeFont(L"Segoe UI Semibold", L"Segoe UI", S(13), FontStyleRegular);
    fSmall = MakeFont(L"Segoe UI",          L"Tahoma",   S(12), FontStyleRegular);
    fIcon  = MakeFont(L"Segoe MDL2 Assets", L"Segoe UI Symbol", S(14), FontStyleRegular);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_menu;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"ShaderOverlay (click for menu)", 128);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

// ---------------------------------------------------------------- entry
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"ShaderOverlay_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"ShaderOverlay is already running (check the tray).", L"ShaderOverlay", MB_ICONINFORMATION);
        return 0;
    }

    typedef BOOL(WINAPI * SetDpiCtx)(HANDLE);
    if (auto fn = (SetDpiCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
        fn((HANDLE)-4);    // PER_MONITOR_AWARE_V2
    else
        SetProcessDPIAware();

    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdipToken, &gsi, nullptr);

    LoadIni();
    if (!SelectOutput(g_monitorIndex) && !SelectOutput(0)) Fail(L"No monitor output found.");
    CreateWindows(hi);
    InitD3D();
    RegisterAll(true);

    SetEffects(g_effectsOn);
    if (g_streamMode) SetStreamMode(true);
    ShowMenu();   // show on first launch so you know it's running

    DWORD lastTop = GetTickCount();
    MSG msg;
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        if (g_petOn && IsWindowVisible(g_menu) && GetTickCount() - g_petTick > 110) {
            g_petTick = GetTickCount();
            PetTick(g_petMin, g_petMax);
            g_uiDirty = true;
        }
        if (g_uiDirty && IsWindowVisible(g_menu)) PaintMenu();

        // Borderless games like to grab topmost; re-assert every half second.
        if (GetTickCount() - lastTop > 500) { lastTop = GetTickCount(); FixZOrder(); StreamHousekeeping(); }

        if (g_effectsOn) RenderFrame();
        else MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
    }

    CapStop();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    for (int i = 0; i < kNumHotkeys; ++i) UnregisterHotKey(g_menu, g_hotkeys[i].id);
    SaveIni();
    delete fTitle; delete fBody; delete fBold; delete fSmall; delete fIcon;
    delete g_bmp;
    GdiplusShutdown(g_gdipToken);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
