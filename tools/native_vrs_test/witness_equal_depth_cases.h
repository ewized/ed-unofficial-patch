#pragma once
#include "../../src/d3d11/native_vrs_witness_state.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace witness_gpu {
enum class RateStep {BeginPass,PrepareWitness,Material,ContinueMaterial,ExpireMaterial,ArmHandoff,AfterHandoff,StrongFull,EqualLeft,EqualRight,EqualFine,EqualMaterial,EqualNext,EqualExpire};
// Three executions per eye start with identical device-local D32S8 bytes:
// combined at full rate, mirrored with real material rates, and original-DSV
// EQUAL with the same rates. Compare after the complete ordered sequence.
template<class Targets,class Rate>
bool equalDepthCases(ID3D11Device* dev,ID3D11DeviceContext* ctx,Targets* targets,ID3D11VertexShader* vertex,
                     ID3D11PixelShader* combined,ID3D11PixelShader* fine,ID3D11PixelShader* material,
                     ID3D11BlendState* blend,Rate& rate,bool hardware) {
    using namespace edvr::native_vrs;
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc=D3D11_COMPARISON_EQUAL;ds.StencilEnable=TRUE;ds.StencilReadMask=ds.StencilWriteMask=255;
    ds.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;
    ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
    ds.BackFace=ds.FrontFace;
    WitnessPtr<ID3D11DepthStencilState> equal,never,stencilWriter,seedState;
    if(FAILED(dev->CreateDepthStencilState(&ds,&equal)))return false;
    ds.BackFace.StencilDepthFailOp=D3D11_STENCIL_OP_REPLACE;
    if(FAILED(dev->CreateDepthStencilState(&ds,&stencilWriter)))return false;
    ds.BackFace=ds.FrontFace;ds.DepthFunc=D3D11_COMPARISON_NEVER;
    if(FAILED(dev->CreateDepthStencilState(&ds,&never)))return false;
    ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
    if(FAILED(dev->CreateDepthStencilState(&ds,&seedState)))return false;
    if(WitnessDepthAccess(targets[0].depth.Get(),stencilWriter.Get()).equalWritable ||
       WitnessDepthAccess(targets[0].depth.Get(),never.Get()).equalWritable)return false;

    D3D11_TEXTURE2D_DESC td{};targets[0].tex[3]->GetDesc(&td);
    const UINT w=td.Width,h=td.Height;
    if(w!=256 || h!=256 || td.Format!=DXGI_FORMAT_R32G8X24_TYPELESS)return false;
    constexpr uint32_t half=0x3f000000,exceptional[]={0,0x80000000,1,0x80000001,0x7fc12345,half-1,half+1};
    // Most pixels accept both coplanar 0.5 draws, including all three rings.
    // The last lines exercise +0, -0, +/- subnormals, NaN and one-ULP misses.
    std::vector<std::array<uint32_t,2>> seed(size_t(w)*h);
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)
        seed[size_t(y)*w+x]={y<h-7?half:exceptional[y-(h-7)],(x+3*y)&255u};
    td.BindFlags=0;D3D11_SUBRESOURCE_DATA data{seed.data(),w*8,0};
    WitnessPtr<ID3D11Texture2D> upload;
    if(FAILED(dev->CreateTexture2D(&td,&data,&upload)))return false;
    WitnessPtr<ID3D11Texture2D> staging[4];
    for(unsigned k=0;k<4;++k){
        targets[0].tex[k]->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(dev->CreateTexture2D(&td,nullptr,&staging[k])))return false;
    }
    constexpr unsigned bpp[]={4,8,16,8};
    auto read=[&](unsigned k,ID3D11Texture2D* src,std::vector<BYTE>& out){
        ctx->CopyResource(staging[k].Get(),src);D3D11_MAPPED_SUBRESOURCE m{};
        if(FAILED(ctx->Map(staging[k].Get(),0,D3D11_MAP_READ,0,&m)))return false;
        const size_t pitch=size_t(w)*bpp[k];out.resize(pitch*h);
        for(UINT y=0;y<h;++y)std::memcpy(out.data()+y*pitch,static_cast<const BYTE*>(m.pData)+y*m.RowPitch,pitch);
        ctx->Unmap(staging[k].Get(),0);return true;
    };
    auto word=[](const BYTE* src){uint32_t value;std::memcpy(&value,src,4);return value;};
    auto scalar=[](const BYTE* src){float value;std::memcpy(&value,src,4);return value;};
    auto fail=[](const char* why,unsigned eye,unsigned late,unsigned pos){
        std::fprintf(stderr,"EQUAL D32S8 %s eye=%u late=%u pixel/index=%u\n",why,eye,late,pos);return false;
    };
    // Seed the second case on the GPU with the same clipped, varying-w
    // triangle used for EQUAL. The readback below supplies the expected
    // depth bits without reconstructing the rasterizer's plane on the CPU.
    if(!rate(RateStep::EqualLeft,targets[0].color.Get(),targets[0].depth.Get()))return false;
    WitnessPtr<ID3D11Texture2D> slopedSeed;WitnessPtr<ID3D11DepthStencilView> slopedView;
    targets[0].tex[3]->GetDesc(&td);
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};targets[0].depth->GetDesc(&vd);
    if(FAILED(dev->CreateTexture2D(&td,nullptr,&slopedSeed)) ||
       FAILED(dev->CreateDepthStencilView(slopedSeed.Get(),&vd,&slopedView)))return false;
    ctx->CopyResource(slopedSeed.Get(),upload.Get());ctx->VSSetShader(vertex,nullptr,0);
    ctx->OMSetRenderTargets(0,nullptr,slopedView.Get());ctx->OMSetDepthStencilState(seedState.Get(),0);
    ctx->PSSetShader(nullptr,nullptr,0);ctx->DrawIndexed(3,9,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);
    std::vector<BYTE> slopeBytes;if(!read(3,slopedSeed.Get(),slopeBytes))return false;
    auto slopeAt=[&](UINT x,UINT y){return scalar(slopeBytes.data()+8*(size_t(y)*w+x));};
    if(!(slopeAt(216,128)-slopeAt(40,128)>.04f && slopeAt(128,40)-slopeAt(128,216)>.08f))
        return fail("missing two-axis GPU depth gradient",0,0,0);

    for(unsigned sloped=0;sloped<2;++sloped)for(unsigned eye=0;eye<2;++eye)for(unsigned late=0;late<2;++late){
        const unsigned base=sloped?9:0;
        std::vector<BYTE> pixels[3][4];
        for(unsigned p=0;p<3;++p){
            auto& t=targets[p];
            const bool split=p!=0,qualified=p==2;
            auto set=[&](RateStep s){return rate(s,t.color.Get(),t.depth.Get());};
            auto full=[&]{return set(eye?RateStep::EqualRight:RateStep::EqualLeft);};
            if(!full())return false;
            ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->CopyResource(t.tex[3].Get(),sloped?slopedSeed.Get():upload.Get());
            std::vector<BYTE> raw;if(!read(3,t.tex[3].Get(),raw))return false;
            for(size_t i=0;i<seed.size();++i)
                if(word(raw.data()+8*i)!=(sloped?word(slopeBytes.data()+8*i):seed[i][0]) || raw[8*i+4]!=seed[i][1])return fail("raw seed",eye,late,unsigned(i));
            const float zero[4]{};for(auto* v:{t.color.Get(),t.slot.Get(),t.skin.Get()})ctx->ClearRenderTargetView(v,zero);
            ID3D11RenderTargetView* rt[8]{};rt[0]=t.color.Get();rt[6]=t.slot.Get();rt[7]=t.skin.Get();
            ctx->OMSetRenderTargets(8,rt,t.depth.Get());ctx->PSSetShader(combined,nullptr,0);
            WitnessMirror mirror;WitnessCounts counts;
            bool measuring=false,ok=true;
            auto fineRate=[&]{return !measuring || set(RateStep::EqualFine);};
            struct Api{
                ID3D11DeviceContext* ctx;decltype(fineRate)& ready;bool full(){return ready();}
                void targets(UINT n,ID3D11RenderTargetView* const* v,ID3D11DepthStencilView* d){ctx->OMSetRenderTargets(n,v,d);}
                void pixel(ID3D11PixelShader* ps){ctx->PSSetShader(ps,nullptr,0);}
                void copy(ID3D11Resource* dst,ID3D11Resource* src){ctx->CopyResource(dst,src);}
            } api{ctx,fineRate};
            for(unsigned phase=0;phase<3;++phase){
                // NEVER warms a real matched mirror and then checks whether a
                // later writable draw is forced to recopy the final scene.
                measuring=phase==1;
                ctx->OMSetDepthStencilState(measuring?equal.Get():never.Get(),0);
                const unsigned length=measuring?3:1;
                if(!split){for(unsigned i=0;i<length;++i)ctx->DrawIndexed(3,base+3*i,0);continue;}
                WitnessPipeline state;
                if(!state.capture(ctx,rt[0],t.depth.Get(),rt[6],rt[7],vertex,combined,fine,material,blend))return false;
                WitnessCohort queue;
                const WitnessCohortKey key{ctx,rt[0],t.depth.Get(),&state,1,1,1,eye};
                for(unsigned i=0;i<length;++i)if(queue.append(key,WitnessDraw::indexed(3,base+3*i,0))!=WitnessCohortAppend::Appended)return false;
                unsigned fineCount=0,materialCount=0;
                auto draw=[&](const WitnessDraw& d,bool scene){
                    WitnessPtr<ID3D11DepthStencilView> actual;ctx->OMGetRenderTargets(0,nullptr,&actual);
                    ok&=actual.Get()==(scene || (qualified && measuring)?t.depth.Get():mirror.view.Get());
                    if(scene){ok&=fineCount==length && d.start==base+3*materialCount;++materialCount;}
                    else{ok&=!materialCount && d.start==base+3*fineCount;++fineCount;}
                    issueWitnessDraw(*ctx,d);
                };
                auto coarse=[&](unsigned i){
                    if(!measuring)return true;
                    if(late && i==2){ok&=set(RateStep::EqualExpire);return false;}
                    const bool accepted=set(i?RateStep::EqualNext:RateStep::EqualMaterial);
                    ok&=accepted;return accepted;
                };
                if(!replayWitnessCohort(queue,state,mirror,1,phase==0?1:phase==1?2:5,counts,api,draw,coarse,
                        [&]{ok&=full();},nullptr,qualified))return fail("replay declined",eye,late,p);
                if(!ok || fineCount!=length || materialCount!=length)return fail("issue order/DSV/rate",eye,late,p);
                if(measuring){
                    if(mirror.reusable!=(p==1 && !late) || !full())return fail("post-EQUAL mirror/strong rate",eye,late,p);
                }
            }
            if(split && (counts.equalEligible!=(qualified?3u:0u) || counts.equalFine!=(qualified?3u:0u) ||
               counts.equalPaired!=(qualified?3u-late:0u) || counts.rateRefused!=late || counts.readOnlyOriginalRuns ||
               counts.copies!=(qualified || late?2u:1u) || counts.copyCauses[D3D11_COMPARISON_NEVER][0]!=counts.copies))return fail("writable accounting",eye,late,p);
            ctx->OMSetRenderTargets(0,nullptr,nullptr);
            for(unsigned k=0;k<4;++k)if(!read(k,t.tex[k].Get(),pixels[p][k]))return false;
        }
        for(size_t i=0;i<seed.size();++i){
            const auto z=sloped?word(slopeBytes.data()+8*i):seed[i][0];const auto* marker=pixels[0][1].data()+8*i;
            const float owner=scalar(marker);
            if(sloped && (owner!=7.f || word(pixels[0][3].data()+8*i)!=z))return fail("GPU-seeded coplanar owner/depth",eye,late,unsigned(i));
            if(!sloped && ((z==half && owner!=7.f) || (z==0 && owner!=5.f) ||
               ((z==0x7fc12345 || z==half-1 || z==half+1) && owner!=0.f)))return fail("hit/miss/last owner",eye,late,unsigned(i));
            if(owner && !(std::fabs(scalar(pixels[0][2].data()+16*i+8)-(owner-1.f)*.5f)<=.001f))
                return fail("skin owner",eye,late,unsigned(i));
            if(pixels[0][3][8*i+4]!=seed[i][1] || (!owner && word(pixels[0][3].data()+8*i)!=z))
                return fail("reference stencil or missed depth",eye,late,unsigned(i));
            for(unsigned k=0;k<4;++k){
                const unsigned n=k==3?5:bpp[k];const size_t offset=i*bpp[k];
                if(std::memcmp(pixels[1][k].data()+offset,pixels[2][k].data()+offset,n) ||
                   ((k || !hardware || (late && owner==7.f)) && std::memcmp(pixels[0][k].data()+offset,pixels[2][k].data()+offset,n)))
                    return fail(k==0?"color":k==1?"motion":k==2?"skin":"depth/stencil",eye,late,unsigned(i));
            }
        }
        if(hardware && !late){
            unsigned seen=0;
            for(UINT y=0;y<h-16;y+=16)for(UINT x=0;x<w;x+=16){
                unsigned rx=0,ry=0;
                auto color=[&](UINT a,UINT b){return word(pixels[2][0].data()+4*(size_t(b)*w+a));};
                for(UINT j=0;j<8;++j)for(UINT i=0;i<8;++i){
                    rx+=i && color(x+i,y+j)==color(x+i-1,y+j);
                    ry+=j && color(x+i,y+j)==color(x+i,y+j-1);
                }
                if(rx==ry){if(!rx)seen|=1;else if(rx==32)seen|=2;else if(rx==48)seen|=4;}
            }
            if(seen!=7)return fail("missing 1x1/2x2/4x4 gradient footprints",eye,late,seen);
        }
    }
    if(hardware)std::puts("{\"equal_writable_gpu_seeded_perspective_depth_and_rings_exact\":true}");
    std::printf("{\"equal_writable_gpu_seeded_perspective_depth_exact\":true,\"physical_rings_checked\":%s,\"raster_depth_dx\":%.6f,\"raster_depth_dy\":%.6f}\n",hardware?"true":"false",
        double(slopeAt(216,128)-slopeAt(40,128)),double(slopeAt(128,40)-slopeAt(128,216)));
    return true;
}
} // namespace witness_gpu
