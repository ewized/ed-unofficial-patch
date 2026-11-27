// Production temporal MV entry: CPU compilation/reflection plus explicit WARP
// pixels. No vendor runtime or game calls; --self-test never creates a device.
#include "../../src/d3d11/temporal_shader_source.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,failures=0;
void check(bool pass,const char* name){++checks;if(!pass){++failures;std::printf("FAIL %s\n",name);}}
void require(HRESULT hr,const char* name){if(FAILED(hr))throw std::runtime_error(name);}
ComPtr<ID3DBlob> compile(const std::string& source,unsigned variant,bool fg){
    const D3D_SHADER_MACRO macros[]={{"EDVR_TEMPORAL_DIAGNOSTICS",variant?"1":"0"},
        {"EDVR_TEMPORAL_TRACE",variant==2?"1":"0"},{"EDVR_FG_MOTION",fg?"1":"0"},{nullptr,nullptr}};
    ComPtr<ID3DBlob> code,error;
    const auto hr=D3DCompile(source.data(),source.size(),"fg_mv_contract",macros,nullptr,"mv","cs_5_0",0,0,&code,&error);
    if(FAILED(hr)&&error)std::printf("%s\n",static_cast<const char*>(error->GetBufferPointer()));
    require(hr,"production mv compile");return code;
}
std::string withoutFg(std::string source){
    for(const char* tag:{"DECL","PARAMS","CAMERA","WRITE"}){
        const std::string begin=std::string("// FG_MOTION_")+tag+"_BEGIN";
        const std::string end=std::string("// FG_MOTION_")+tag+"_END";
        const auto a=source.find(begin),b=source.find(end);
        if(a==std::string::npos||b==std::string::npos)throw std::runtime_error("FG source markers");
        source.erase(a,b+end.size()-a);
    }
    return source;
}
void binding(ID3DBlob* code,bool present){
    ComPtr<ID3D11ShaderReflection> ref;
    require(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&ref)),"reflect mv");
    D3D11_SHADER_INPUT_BIND_DESC desc{};
    const auto hr=ref->GetResourceBindingDescByName("FGPhysicalMotion",&desc);
    check(present?SUCCEEDED(hr)&&desc.BindPoint==1&&desc.BindCount==1&&desc.Type==D3D_SIT_UAV_RWTYPED:FAILED(hr),
        "FG variants alone expose the typed u1 physical guide");
    check(FAILED(ref->GetResourceBindingDescByName("N",&desc)),"mv entry eliminates main history u1 alias");
}
struct Params {
    int32_t region[4]{},size[2]{},texSize[2]{};
    float tanNow[4]{},tanPrev[4]{},jit[4]{},dR0[4]{},dR1[4]{},dR2[4]{};
    float cand[4][3][4]{},blend=0,gamma=0;int32_t haveHistory=0,candMask=0;
    float knobs[4]{},tvUsed[4]{},tvCand[4]{},tvCam[4]{},split[4]{},probe[4]{},holoJitter[4]{};
    float fgWorld[3][4]{};
};
static_assert(sizeof(Params)==496,"actual temporal constant layout");
constexpr unsigned W=16,H=8;
struct Texture {
    ComPtr<ID3D11Texture2D> gpu,stage;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> srv;
};
struct Rig {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Buffer> constants,stats;ComPtr<ID3D11UnorderedAccessView> statsUav;
    Texture fg,sr,depth,mask,trace,mark,color,sceneDepth;
    Texture engineSlots, skinMotion;
    ComPtr<ID3D11Buffer> enginePool, engineScene;
    ComPtr<ID3D11ShaderResourceView> poolView;
    Texture texture(DXGI_FORMAT format,unsigned binds,bool staging=false){
        Texture out;D3D11_TEXTURE2D_DESC d{};d.Width=W;d.Height=H;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format;d.BindFlags=binds;
        require(device->CreateTexture2D(&d,nullptr,&out.gpu),"texture");
        if(binds&D3D11_BIND_UNORDERED_ACCESS)require(device->CreateUnorderedAccessView(out.gpu.Get(),nullptr,&out.uav),"UAV");
        if(binds&D3D11_BIND_SHADER_RESOURCE)require(device->CreateShaderResourceView(out.gpu.Get(),nullptr,&out.srv),"SRV");
        if(staging){d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            require(device->CreateTexture2D(&d,nullptr,&out.stage),"staging");}
        return out;
    }
    Rig(){
        D3D_FEATURE_LEVEL level{};
        require(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"WARP");
        D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(Params);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        require(device->CreateBuffer(&d,nullptr,&constants),"constants");
        d.ByteWidth=128*4;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=4;
        require(device->CreateBuffer(&d,nullptr,&stats),"stats");require(device->CreateUnorderedAccessView(stats.Get(),nullptr,&statsUav),"stats UAV");
        fg=texture(DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_UNORDERED_ACCESS,true);
        sr=texture(DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_UNORDERED_ACCESS,true);
        depth=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
        mask=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
        trace=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
        mark=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        color=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        sceneDepth=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        const std::array<float,W*H*4> zero{};
        context->UpdateSubresource(color.gpu.Get(),0,nullptr,zero.data(),W*16,0);
        context->UpdateSubresource(sceneDepth.gpu.Get(),0,nullptr,zero.data(),W*4,0);
    }
    // One skinned record at the last foreground texel. Its previous
    // position is 37.5 cm to the left; adjacent centers remain unmarked.
    void makeDepthEdge() {
        engineSlots=texture(DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        skinMotion=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        std::array<float,W*H> depths{};
        for(unsigned y=0;y<H;++y) for(unsigned x=0;x<W;++x) depths[y*W+x]=x<W/2?.25f:.015625f;
        context->UpdateSubresource(sceneDepth.gpu.Get(),0,nullptr,depths.data(),W*4,0);
        const unsigned pixel=(H/2)*W+W/2-1;
        std::array<float,W*H*2> slots{};
        slots[2*pixel]=1; slots[2*pixel+1]=depths[pixel];
        context->UpdateSubresource(engineSlots.gpu.Get(),0,nullptr,slots.data(),W*8,0);
        std::array<float,W*H*4> skin{};
        skin[4*pixel]=-37.5f; skin[4*pixel+3]=1;
        context->UpdateSubresource(skinMotion.gpu.Get(),0,nullptr,skin.data(),W*16,0);
        std::array<uint32_t,21*4> record{};
        record[0]=1; // nonzero palette base selects the skinned reprojection
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth=desc.StructureByteStride=sizeof(record);
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem=record.data();
        require(device->CreateBuffer(&desc,&initial,&enginePool),"engine pool");
        require(device->CreateShaderResourceView(enginePool.Get(),nullptr,&poolView),"engine pool view");
        std::array<float,277*4> rows{};
        rows[270*4]=rows[271*4+1]=rows[272*4+3]=1; rows[273*4+2]=.25f;
        desc={}; desc.ByteWidth=sizeof(rows); desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        initial.pSysMem=rows.data();
        require(device->CreateBuffer(&desc,&initial,&engineScene),"engine projection rows");
    }
    std::vector<uint16_t> read(Texture& image){
        context->CopyResource(image.stage.Get(),image.gpu.Get());D3D11_MAPPED_SUBRESOURCE m{};
        require(context->Map(image.stage.Get(),0,D3D11_MAP_READ,0,&m),"readback");
        std::vector<uint16_t> out(W*H*2);
        for(unsigned y=0;y<H;++y)std::memcpy(out.data()+y*W*2,static_cast<const uint8_t*>(m.pData)+y*m.RowPitch,W*4);
        context->Unmap(image.stage.Get(),0);return out;
    }
    std::array<std::vector<uint16_t>,2> run(ID3DBlob* bytecode,bool invalid,bool fresh,bool edge=false,float fgTranslation=.25f){
        ComPtr<ID3D11ComputeShader> shader;
        require(device->CreateComputeShader(bytecode->GetBufferPointer(),bytecode->GetBufferSize(),nullptr,&shader),"create actual mv");
        Params p{};p.region[2]=p.size[0]=p.texSize[0]=W;p.region[3]=p.size[1]=p.texSize[1]=H;
        const float now[4]={-1,1,-1,1},before[4]={-1.25f,.75f,-1,1};
        std::memcpy(p.tanNow,now,sizeof(now));std::memcpy(p.tanPrev,before,sizeof(before));
        p.dR0[0]=p.dR1[1]=1;p.dR2[2]=invalid?-1.f:1.f;p.probe[2]=1;
        if(edge){
            std::memcpy(p.tanPrev,now,sizeof(now));
            p.knobs[1]=1; p.knobs[2]=.25f; p.split[0]=.001f;
            p.tvUsed[0]=p.tvCam[0]=.25f; p.tvCam[3]=1;
            p.cand[2][0][0]=p.cand[2][1][1]=p.cand[2][2][2]=1;
            p.fgWorld[0][0]=p.fgWorld[1][1]=p.fgWorld[2][2]=1; p.fgWorld[0][3]=fgTranslation;
            p.holoJitter[2]=1; p.probe[3]=64+256+2048+16384;
        }
        context->UpdateSubresource(constants.Get(),0,nullptr,&p,0,0);
        std::array<float,W*H> marks{};marks.fill((fresh?6.f:2.f)/255.f);
        context->UpdateSubresource(mark.gpu.Get(),0,nullptr,marks.data(),W*4,0);
        std::array<uint16_t,W*H*2> sentinel{};
        for(unsigned i=0;i<W*H;++i){sentinel[i*2]=0xc400;sentinel[i*2+1]=0xc800;}
        context->UpdateSubresource(fg.gpu.Get(),0,nullptr,sentinel.data(),W*4,0);
        context->UpdateSubresource(sr.gpu.Get(),0,nullptr,sentinel.data(),W*4,0);
        const UINT zero[4]{};context->ClearUnorderedAccessViewUint(statsUav.Get(),zero);
        ID3D11ShaderResourceView* srvs[5]={color.srv.Get(),color.srv.Get(),sceneDepth.srv.Get(),nullptr,mark.srv.Get()};
        ID3D11UnorderedAccessView* uavs[8]={nullptr,fg.uav.Get(),statsUav.Get(),sr.uav.Get(),depth.uav.Get(),mask.uav.Get(),nullptr,trace.uav.Get()};
        context->CSSetShader(shader.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,constants.GetAddressOf());
        context->CSSetShaderResources(0,5,srvs);context->CSSetUnorderedAccessViews(0,8,uavs,nullptr);
        if(edge){
            ID3D11ShaderResourceView* engineViews[]={engineSlots.srv.Get(),poolView.Get(),skinMotion.srv.Get()};
            ID3D11Buffer* engineBuffers[]={engineScene.Get(),engineScene.Get()};
            context->CSSetShaderResources(21,3,engineViews);
            context->CSSetConstantBuffers(1,2,engineBuffers);
        }
        context->Dispatch((W+7)/8,(H+7)/8,1);
        ID3D11UnorderedAccessView* none[8]{};context->CSSetUnorderedAccessViews(0,8,none,nullptr);
        ID3D11ShaderResourceView* noSrvs[5]{};context->CSSetShaderResources(0,5,noSrvs);
        return {read(sr),read(fg)};
    }
};
bool pixels(const std::vector<uint16_t>& data,uint16_t x,uint16_t y){
    for(unsigned i=0;i<W*H;++i)if(data[2*i]!=x||data[2*i+1]!=y)return false;return true;
}
std::string replaceOne(std::string source,const char* from,const char* to){
    const auto at=source.find(from);if(at==std::string::npos)throw std::runtime_error("mutation anchor");
    source.replace(at,std::strlen(from),to);return source;
}
}
int main(int argc,char** argv){
    if(argc!=2||(std::strcmp(argv[1],"--self-test")&&std::strcmp(argv[1],"--warp")))return 2;
    const bool warp=std::strcmp(argv[1],"--warp")==0;
    try{
        const std::string source=edvr::kTemporalCsHlsl;const auto legacy=withoutFg(source);
        std::array<ComPtr<ID3DBlob>,3> off,on;
        for(unsigned v=0;v<3;++v){
            off[v]=compile(source,v,false);on[v]=compile(source,v,true);binding(off[v].Get(),false);binding(on[v].Get(),true);
            const auto old=compile(legacy,v,false);
            check(old->GetBufferSize()==off[v]->GetBufferSize()&&!std::memcmp(old->GetBufferPointer(),off[v]->GetBufferPointer(),old->GetBufferSize()),
                "FG Off is byte-exact with production HLSL without FG additions for fast/diagnostic/trace");
        }
        if(warp){
            Rig rig;
            rig.makeDepthEdge();
            for(unsigned v=0;v<3;++v){
                const auto a=rig.run(off[v].Get(),false,true),b=rig.run(on[v].Get(),false,true);
                check(a[0]==b[0]&&pixels(b[0],0x5000,0x4c00),"SR rejection size*2 remains exact with FG On");
                check(pixels(a[1],0xc400,0xc800),"FG Off does not touch bound u1 sentinel");
                check(pixels(b[1],0x4000,0),"FG preserves actual physical2px motion despite fresh UI SR rejection");
                const auto invalid=rig.run(on[v].Get(),true,true);
                check(pixels(invalid[1],0x7bff,0x7bff),"invalid projection writes exact RG16 65504 sentinel");
                const auto stable=rig.run(on[v].Get(),false,false);
                check(pixels(stable[0],0x4000,0)&&stable[0]==stable[1],"ordinary valid motion remains identical between SR and FG");
                const auto edge=rig.run(on[v].Get(),false,false,true);
                const auto xAt=[](const std::vector<uint16_t>& plane,unsigned x){return plane[2*((H/2)*W+x)];};
                check(xAt(edge[1],W/2-2)==0x4000 && xAt(edge[1],W/2)==0x3000,
                      "FG uses center depth: foreground moves 2 px, adjacent background moves 1/8 px");
                const auto switched=rig.run(on[v].Get(),false,false,true,0);
                check(xAt(switched[1],W/2)==0 && xAt(switched[0],W/2)==0x4000,
                      "FG uses the stationary target-eye pose when the SR camera has moved");
                check(xAt(edge[0],W/2)==0x4000,"SR retains its 3x3 foreground motion at the edge");
                check(xAt(edge[1],W/2-1)==0xc200 && xAt(edge[0],W/2-1)==0xc200,
                      "the marked foreground center takes the engine record's -3 px object motion");
            }
            const auto reactive=compile(replaceOne(source,"fgValid ? fgMotion :", "fgValid ? float2(size)*2 :"),0,true);
            check(!pixels(rig.run(reactive.Get(),false,true)[1],0x4000,0),"mutation importing SR rejection into FG is detected by pixel judge");
            const auto invalid=compile(replaceOne(source,"float2(65504.0, 65504.0)","float2(0.0, 0.0)"),0,true);
            check(!pixels(rig.run(invalid.Get(),true,true)[1],0x7bff,0x7bff),"mutation losing invalid sentinel is detected by pixel judge");
        }
    }catch(const std::exception& e){++failures;std::printf("FAIL exception: %s\n",e.what());}
    std::printf("FG physical MV actual HLSL: checks=%u failures=%u WARP=%u vendor=0\n",checks,failures,warp?1u:0u);
    return failures?1:0;
}
