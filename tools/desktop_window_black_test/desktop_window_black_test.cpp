// The production clear on WARP: every pixel black, no draw/state setup, and a
// no-clear negative control that must leave the original coloured desktop.
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include "../../src/d3d11/desktop_window_black.h"
using Microsoft::WRL::ComPtr;
int failures = 0;
void check(bool ok, const char* text) { if (!ok) { std::printf("FAIL %s\n", text); ++failures; } }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "--dry-run")) { std::puts("black desktop: WARP clear/state regression, no window or game"); return 0; }
    if (std::strcmp(argv[1], "--self-test")) return 2;
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx))) return 1;
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> back,sentinel,read;
    ComPtr<ID3D11RenderTargetView> target,held;
    if(FAILED(dev->CreateTexture2D(&td,nullptr,&back)) || FAILED(dev->CreateTexture2D(&td,nullptr,&sentinel)) ||
       FAILED(dev->CreateRenderTargetView(back.Get(),nullptr,&target)) || FAILED(dev->CreateRenderTargetView(sentinel.Get(),nullptr,&held)))return 1;
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(dev->CreateTexture2D(&td,nullptr,&read)))return 1;
    ID3D11RenderTargetView* bound=held.Get();ctx->OMSetRenderTargets(1,&bound,nullptr);
    const D3D11_VIEWPORT vp{1,2,3,4,0,1};ctx->RSSetViewports(1,&vp);
    auto isBlack=[&] {
        ctx->CopyResource(read.Get(),back.Get());D3D11_MAPPED_SUBRESOURCE m{};
        if(FAILED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&m)))return false;
        bool ok=true;
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
            const auto* p=static_cast<const unsigned char*>(m.pData)+y*m.RowPitch+4*x;
            ok=ok && p[0]==0 && p[1]==0 && p[2]==0 && p[3]==255;
        }
        ctx->Unmap(read.Get(),0);return ok;
    };
    const float red[4]={1,0,0,1};ctx->ClearRenderTargetView(target.Get(),red);
    check(!isBlack(),"negative control: omitting the clear leaves coloured desktop pixels");
    edvr::desktopWindowClearBlack(ctx.Get(),target.Get());
    check(isBlack(),"production clear makes every pixel opaque black despite a smaller viewport");
    ComPtr<ID3D11RenderTargetView> after;ctx->OMGetRenderTargets(1,&after,nullptr);
    D3D11_VIEWPORT afterVp{};UINT n=1;ctx->RSGetViewports(&n,&afterVp);
    check(after.Get()==held.Get() && n==1 && !std::memcmp(&afterVp,&vp,sizeof vp),"clear leaves bindings and viewport unchanged");
    std::printf("black desktop: %d failures\n",failures);return failures?1:0;
}
