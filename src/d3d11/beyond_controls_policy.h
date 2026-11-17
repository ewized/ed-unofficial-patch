#pragma once
#include <cmath>
#include <cstdlib>
namespace edvr { namespace beyond_controls {
struct Preset {const char* value;const char* label;};
inline constexpr Preset inputPresets[]={{"1.0","Full / DLAA"},{"0.666667","Quality"},{"0.58","Balanced"},{"0.5","Performance"},{"0.3333333333","Ultra Performance"}};
inline int inputPreset(double v){for(int i=0;i<5;++i)if(std::fabs(v-std::atof(inputPresets[i].value))<.00001)return i;return -1;}
inline int stepPreset(int current,int direction){return current<0?0:(current+(direction<0?4:1))%5;}
inline const char* vrsMode(unsigned mode){const char* names[]={"Off","Maximum","Balanced","Minimum","Debug"};return mode<5?names[mode]:"Unknown";}
inline const char* nvidiaLabel(bool observed,bool upscale,double inputRatio){
    if(observed)return upscale?"DLSS (observed)":"DLAA (observed)";
    if(!std::isfinite(inputRatio)||inputRatio<=0)return "NVIDIA (requested)";
    return inputRatio<1.0?"DLSS (estimated)":"DLAA (estimated)";
}
} }
