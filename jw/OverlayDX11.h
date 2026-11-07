#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>

class OverlayDX11 {
public:
    OverlayDX11();
    ~OverlayDX11();

    bool Init(int width, int height);

    HWND                 GetHwnd()    const { return hwnd_; }
    ID3D11Device*        GetDevice()  const { return device_; }
    ID3D11DeviceContext* GetContext() const { return context_; }
    int                  GetWidth()   const { return width_; }
    int                  GetHeight()  const { return height_; }

    void BeginFrame(bool solidBlack = false);
    void EndFrame();
    bool Resize(int width, int height);

private:
    HWND                  hwnd_     = nullptr;
    ID3D11Device*         device_   = nullptr;
    ID3D11DeviceContext*  context_  = nullptr;
    IDXGISwapChain1*      swap_     = nullptr;
    ID3D11RenderTargetView* rtv_    = nullptr;
    IDCompositionDevice*  dcomp_    = nullptr;
    IDCompositionTarget*  dcompTgt_ = nullptr;
    IDCompositionVisual*  dcompVis_ = nullptr;
    int                   width_    = 0;
    int                   height_   = 0;
    bool                  allowTearing_ = false;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
};
