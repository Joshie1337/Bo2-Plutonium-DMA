#include "OverlayDX11.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include <stdio.h>
#include <ctime>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static std::wstring MakeRandomW(int len) {
    static const wchar_t alphanum[] = L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::wstring s; s.reserve(len);
    srand((unsigned)time(nullptr) ^ 0xC0FFEE);
    for (int i = 0; i < len; ++i) s += alphanum[rand() % (sizeof(alphanum)/sizeof(wchar_t) - 1)];
    return s;
}

extern void UpdateMonitorsList();
extern UINT g_ResizeWidth;
extern UINT g_ResizeHeight;

LRESULT CALLBACK OverlayDX11::WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l))
        return TRUE;
    switch (msg) {
    case WM_DISPLAYCHANGE:
        UpdateMonitorsList();
        break;
    case WM_SIZE:
        if (w == SIZE_MINIMIZED) break;
        g_ResizeWidth  = (UINT)LOWORD(l);
        g_ResizeHeight = (UINT)HIWORD(l);
        break;
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

OverlayDX11::OverlayDX11() {}

OverlayDX11::~OverlayDX11() {
    if (rtv_)      { rtv_->Release();      rtv_ = nullptr; }
    if (dcompVis_) { dcompVis_->Release(); dcompVis_ = nullptr; }
    if (dcompTgt_) { dcompTgt_->Release(); dcompTgt_ = nullptr; }
    if (dcomp_)    { dcomp_->Release();    dcomp_ = nullptr; }
    if (swap_)     { swap_->Release();     swap_ = nullptr; }
    if (context_)  { context_->Release();  context_ = nullptr; }
    if (device_)   { device_->Release();   device_ = nullptr; }
    if (hwnd_)     { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool OverlayDX11::Init(int width, int height) {
    width_  = width;
    height_ = height;

    static std::wstring cls = MakeRandomW(16);
    static std::wstring ttl = MakeRandomW(16);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls.c_str();
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_NOACTIVATE,
        cls.c_str(), ttl.c_str(),
        WS_POPUP,
        0, 0, width, height,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) return false;

    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    ShowWindow(hwnd_, SW_SHOWDEFAULT);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                flags, fls, _countof(fls), D3D11_SDK_VERSION,
                &device_, &fl, &context_))) {
        return false;
    }

    IDXGIDevice*  dxgiDev    = nullptr;
    IDXGIAdapter* dxgiAdpt   = nullptr;
    IDXGIFactory2* dxgiFact  = nullptr;
    if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return false;
    dxgiDev->GetAdapter(&dxgiAdpt);
    dxgiAdpt->GetParent(__uuidof(IDXGIFactory2), (void**)&dxgiFact);

    {
        BOOL t = FALSE;
        IDXGIFactory5* f5 = nullptr;
        if (SUCCEEDED(dxgiFact->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5)) && f5) {
            f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof(t));
            f5->Release();
        }
        allowTearing_ = !!t;
    }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width              = (UINT)width;
    sd.Height             = (UINT)height;
    sd.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count   = 1;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount        = 2;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode          = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags              = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    if (FAILED(dxgiFact->CreateSwapChainForComposition(device_, &sd, nullptr, &swap_))) {
        dxgiFact->Release(); dxgiAdpt->Release(); dxgiDev->Release();
        return false;
    }

    dxgiFact->Release(); dxgiAdpt->Release(); dxgiDev->Release();

    if (FAILED(DCompositionCreateDevice(nullptr, __uuidof(IDCompositionDevice),
              (void**)&dcomp_))) return false;
    if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTgt_))) return false;
    if (FAILED(dcomp_->CreateVisual(&dcompVis_))) return false;
    dcompVis_->SetContent(swap_);
    dcompTgt_->SetRoot(dcompVis_);
    dcomp_->Commit();

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    if (FAILED(device_->CreateRenderTargetView(bb, nullptr, &rtv_))) {
        bb->Release();
        return false;
    }
    bb->Release();

    return true;
}

void OverlayDX11::BeginFrame(bool solidBlack) {
    float clearColor[4] = { 0.f, 0.f, 0.f, 0.f }; // fully transparent
    if (solidBlack) {
        clearColor[3] = 1.0f; // solid black
    }
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clearColor);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.f; vp.TopLeftY = 0.f;
    vp.Width    = (float)width_;
    vp.Height   = (float)height_;
    vp.MinDepth = 0.f; vp.MaxDepth = 1.f;
    context_->RSSetViewports(1, &vp);
}

void OverlayDX11::EndFrame() {
    UINT flags = allowTearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
    swap_->Present(0, flags);
}

bool OverlayDX11::Resize(int width, int height) {
    if (!swap_ || width <= 0 || height <= 0) return false;
    if (width == width_ && height == height_) return true;

    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    context_->OMSetRenderTargets(0, nullptr, nullptr);

    UINT flags = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = swap_->ResizeBuffers(0, (UINT)width, (UINT)height,
                                      DXGI_FORMAT_UNKNOWN, flags);
    if (FAILED(hr)) return false;

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    HRESULT hr2 = device_->CreateRenderTargetView(bb, nullptr, &rtv_);
    bb->Release();
    if (FAILED(hr2)) return false;

    width_  = width;
    height_ = height;
    return true;
}
