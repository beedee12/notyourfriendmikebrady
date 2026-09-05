#include <windows.h>
#include <cstdint>
#include <memory>
#include "uevr/Plugin.hpp"
using namespace uevr;

namespace {
using Fn=int32_t(__fastcall*)(void*,bool,uint32_t);
Fn orig=nullptr; void** vt=nullptr; bool hooked=false; int logs=0;

int32_t __fastcall hook(void* self,bool stereo,uint32_t idx){
    int32_t o=orig?orig(self,stereo,idx):0;
    if(!stereo) return o;

    // V6: do NOT invent eye assignment.
    // Preserve UEVR/engine's genuine LEFT/RIGHT classifications (1/2),
    // which is the state where tracking survives.
    // Only neutralize classifications outside that canonical pair.
    int32_t r=o;
    if(o!=1 && o!=2) r=0;

    if(logs++<160)
        API::get()->log_info("[SHIPFIX6] request=%u originalPass=%d result=%d",idx,o,r);
    return r;
}

void install(UEVR_StereoRenderingDeviceHandle d){
    if(hooked||!d)return;
    auto*** p=reinterpret_cast<void***>(d);
    if(!p||!*p)return;
    vt=*p;
    if(!vt[5])return;
    DWORD old{};
    if(!VirtualProtect(&vt[5],sizeof(void*),PAGE_EXECUTE_READWRITE,&old))return;
    orig=reinterpret_cast<Fn>(vt[5]);
    vt[5]=reinterpret_cast<void*>(&hook);
    DWORD x{};
    VirtualProtect(&vt[5],sizeof(void*),old,&x);
    FlushInstructionCache(GetCurrentProcess(),&vt[5],sizeof(void*));
    hooked=true;
    API::get()->log_info("[SHIPFIX6] hooked GetViewPassForIndex; preserving only native LEFT/RIGHT passes");
}
void cb(UEVR_StereoRenderingDeviceHandle d,int,float,UEVR_Vector3f*,UEVR_Rotatorf*,bool){install(d);}
}

class Fix final:public Plugin{
public:void on_initialize()override{
    API::get()->param()->sdk->callbacks->on_early_calculate_stereo_view_offset(&cb);
    API::get()->log_info("[SHIPFIX6] loaded");
}};
std::unique_ptr<Fix> g{new Fix()};
