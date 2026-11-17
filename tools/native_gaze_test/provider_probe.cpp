// Explicit opt-in metadata probe; links native_gaze.cpp without a Scene application.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include "../../src/d3d11/native_gaze.h"

DWORD WINAPI watchdog(void* event) {
    if(WaitForSingleObject(static_cast<HANDLE>(event),20000)==WAIT_TIMEOUT) {
        std::fputs("{\"error\":\"provider probe deadline 20000ms\"}\n",stderr);
        std::fflush(nullptr);TerminateProcess(GetCurrentProcess(),124);
    }
    return 0;
}
int wmain(int argc,wchar_t** argv) {
    if(argc==1||(argc==2&&!wcscmp(argv[1],L"--dry-run"))) {
        std::puts("{\"mode\":\"DRY\",\"provider_calls\":0,\"dynamic_dll_loads\":0,\"required_cli\":\"--run\"}");return 0;
    }
    if(argc!=2||wcscmp(argv[1],L"--run")) {
        std::fputs("usage: --dry-run | --run\n",stderr);return 2;
    }
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!event)return 2;
    HANDLE thread=CreateThread(nullptr,0,watchdog,event,0,nullptr);
    if(!thread){CloseHandle(event);return 2;}
    const auto sample=edvr::nativeGazePoll(true);
    const bool projectionPair=sample.eyes[0].projectionValid&&sample.eyes[1].projectionValid;
    std::printf("{\"provider_queried\":%s,\"provider_returned_valid\":%s,\"usable\":%s,\"failure\":%u,\"eye_projection_parallel_valid\":[%s,%s],\"projection_parallel_pair_valid\":%s,\"source_support\":\"%s\",\"receipt_qpc\":%llu,\"receipt_frequency\":%llu,\"sensor_timestamp_unavailable\":%s,\"physical_gaze_claim\":false}\n",
        sample.providerQueried?"true":"false",sample.providerReturnedValid?"true":"false",sample.usable?"true":"false",static_cast<unsigned>(sample.failure),
        sample.eyes[0].projectionValid?"true":"false",sample.eyes[1].projectionValid?"true":"false",projectionPair?"true":"false",
        sample.usable&&projectionPair?"observed":"unknown",static_cast<unsigned long long>(sample.receiptQpc),static_cast<unsigned long long>(sample.receiptQpcFrequency),sample.sensorTimestampUnavailable?"true":"false");
    SetEvent(event);WaitForSingleObject(thread,1000);CloseHandle(thread);CloseHandle(event);
    // Missing permission/provider data is an unknown observation, not incompatibility.
    return 0;
}
