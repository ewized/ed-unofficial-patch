#pragma once
#include <windows.h>
#include <string>
#include <cstdint>

namespace edvr { namespace beyond_vrs {
struct Config { unsigned mode=0, ignored=0, invert=0; };
struct Snapshot {
    Config value;
    bool valid=false, shimLoaded=false, magicLoaded=false;
    DWORD error=ERROR_NOT_READY;
};
inline bool parse(const std::string& text, Config& out) {
    out=Config{}; Config value; unsigned seen=0;
    if(text.empty()||text.size()>4096)return false;
    for(unsigned char c:text)if((c<32&&c!='\t'&&c!='\r'&&c!='\n')||c>126)return false;
    size_t pos=0;
    while(pos<text.size()) {
        const size_t begin=pos;while(pos<text.size()&&text[pos]!='\r'&&text[pos]!='\n')++pos;
        size_t end=pos;
        if(pos<text.size()&&text[pos]=='\r'){++pos;if(pos==text.size()||text[pos]!='\n')return false;}
        if(pos<text.size()&&text[pos]=='\n')++pos;
        size_t first=begin;while(first<end&&(text[first]==' '||text[first]=='\t'))++first;
        while(end>first&&(text[end-1]==' '||text[end-1]=='\t'))--end;
        if(first==end)continue;
        const size_t eq=text.find('=',first);if(eq>=end)return false;
        size_t keyEnd=eq;while(keyEnd>first&&(text[keyEnd-1]==' '||text[keyEnd-1]=='\t'))--keyEnd;
        size_t number=eq+1;while(number<end&&(text[number]==' '||text[number]=='\t'))++number;
        if(number+1!=end||text[number]<'0'||text[number]>'4')return false;
        const unsigned n=unsigned(text[number]-'0');const auto key=text.substr(first,keyEnd-first);unsigned bit=0;
        if(key=="mode"){bit=1;value.mode=n;}
        else if(key=="ignore_eye_tracking"&&n<=1){bit=2;value.ignored=n;}
        else if(key=="invert_y_axis"&&n<=1){bit=4;value.invert=n;}
        else return false;
        if(seen&bit)return false;seen|=bit;
    }
    if(seen!=7)return false;out=value;return true;
}
inline std::string serialize(const Config& c) {
    if(c.mode>4||c.ignored>1||c.invert>1)return {};
    return "mode="+std::to_string(c.mode)+"\nignore_eye_tracking="+std::to_string(c.ignored)+"\ninvert_y_axis="+std::to_string(c.invert)+"\n";
}
inline unsigned field(const Config& c,unsigned which){return which==0?c.mode:which==1?c.ignored:c.invert;}
inline bool setField(Config& c,unsigned which,unsigned value){
    if(which>2||value>(which==0?4u:1u))return false;
    if(which==0)c.mode=value;else if(which==1)c.ignored=value;else c.invert=value;return true;
}
inline std::wstring controlPath() {
    wchar_t exe[32768]{};const DWORD n=GetModuleFileNameW(nullptr,exe,32768);
    if(!n||n>=32768)return {};
    std::wstring path(exe,n);const size_t slash=path.find_last_of(L"\\/");
    if(slash==std::wstring::npos||_wcsicmp(path.c_str()+slash+1,L"EliteDangerous64.exe"))return {};
    return path.substr(0,slash+1)+L"EliteBeyondVRS\\foveation.cfg";
}
// Worker thread only. No DLL load, provider query, driver call or coordinate read.
inline Snapshot readAt(const std::wstring& path) {
    Snapshot s;s.shimLoaded=GetModuleHandleW(L"libpvrclient64.dll")!=nullptr;
    s.magicLoaded=GetModuleHandleW(L"LibMagicD3D1164.dll")!=nullptr;
    if(path.empty()){s.error=ERROR_INVALID_NAME;return s;}
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE){s.error=GetLastError();return s;}
    char bytes[4096];DWORD n=GetFileSize(h,nullptr),got=0;
    const bool ok=GetFileType(h)==FILE_TYPE_DISK&&n&&n<=sizeof(bytes)&&ReadFile(h,bytes,n,&got,nullptr)&&got==n&&GetFileSize(h,nullptr)==n;
    s.error=ok?ERROR_SUCCESS:ERROR_INVALID_DATA;CloseHandle(h);
    if(ok&&parse(std::string(bytes,got),s.value)){s.valid=true;return s;}
    s.value=Config{};s.error=ERROR_INVALID_DATA;return s;
}
inline Snapshot read(){return readAt(controlPath());}
// A complete three-field file, flushed beside the destination then replaced.
// Kept separate from the path anchor so the offline rig can use its own temp directory.
inline bool publishAt(const std::wstring& path,const Config& value,DWORD& error) {
    const std::string bytes=serialize(value);if(path.empty()||bytes.empty()){error=ERROR_INVALID_PARAMETER;return false;}
    const auto slash=path.find_last_of(L"\\/");if(slash==std::wstring::npos){error=ERROR_INVALID_NAME;return false;}
    const std::wstring dir=path.substr(0,slash);
    if(!CreateDirectoryW(dir.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){error=GetLastError();return false;}
    const std::wstring temp=path+L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
    HANDLE h=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE){error=GetLastError();return false;}
    DWORD written=0;bool ok=WriteFile(h,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(h);
    error=ok?ERROR_SUCCESS:GetLastError();if(!CloseHandle(h)&&ok){ok=false;error=GetLastError();}
    if(ok&&!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){ok=false;error=GetLastError();}
    if(!ok)DeleteFileW(temp.c_str());return ok;
}
inline bool writeFieldAt(const std::wstring& path,unsigned which,unsigned value,Snapshot& after,std::string& why) {
    after=readAt(path);
    // A missing file can be initialized from the fail-closed OFF defaults;
    // malformed existing input is preserved for inspection, never silently repaired.
    if(!after.valid&&after.error!=ERROR_FILE_NOT_FOUND&&after.error!=ERROR_PATH_NOT_FOUND){why="VRS file unavailable or invalid ("+std::to_string(after.error)+")";return false;}
    Config next=after.value;if(!setField(next,which,value)){why="invalid VRS control value";return false;}
    DWORD error=0;if(!publishAt(path,next,error)){why="VRS save failed ("+std::to_string(error)+")";return false;}
    after=readAt(path);if(!after.valid){why="VRS readback failed";return false;}
    if(serialize(after.value)!=serialize(next)){why="VRS file changed during save";return false;}
    return true;
}
inline bool writeField(unsigned which,unsigned value,Snapshot& after,std::string& why) {
    return writeFieldAt(controlPath(),which,value,after,why);
}
} }
