#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <initializer_list>
#include "../../src/d3d11/beyond_controls_policy.h"
#include "../../src/d3d11/beyond_vrs_control.h"
static unsigned checks=0,failures=0;
static void check(bool v,const char* name){++checks;if(!v){++failures;std::printf("FAIL %s\n",name);}}
static std::string readBytes(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char** argv) {
    if(argc==2&&std::string(argv[1])=="--dry-run"){std::puts("Beyond controls: policy/parser and isolated temp-file atomic publication; no game/input/driver/GPU calls");return 0;}
    using namespace edvr::beyond_vrs;using namespace edvr::beyond_controls;
    for(unsigned m=0;m<5;++m)for(unsigned i=0;i<2;++i)for(unsigned y=0;y<2;++y){
        Config original{m,i,y},parsed;const auto text=serialize(original);
        check(parse(text,parsed)&&parsed.mode==m&&parsed.ignored==i&&parsed.invert==y,"every supported triple round-trips");
        check(text.find('\r')==std::string::npos&&text.find('#')==std::string::npos,"publication is plain ASCII without comments");
    }
    const std::string good="mode=2\nignore_eye_tracking=0\ninvert_y_axis=1\n";
    Config parsed;
    for(const auto& text:std::initializer_list<std::string>{"", "mode=1\n", good+"mode=0\n",good+"unknown=0\n",good+"#comment\n",std::string("\xef\xbb\xbf")+good,
          "mode=5\nignore_eye_tracking=0\ninvert_y_axis=0\n","mode=1\nignore_eye_tracking=2\ninvert_y_axis=0\n", "mode=1\rignore_eye_tracking=0\ninvert_y_axis=0\n",good+std::string(4096,' '),good+std::string(1,'\0')}) {
        parsed={4,1,1};check(!parse(text,parsed)&&parsed.mode==0&&!parsed.ignored&&!parsed.invert,"malformed input fails entirely OFF");
    }
    check(parse("\r\n mode = 4\r\nignore_eye_tracking = 1\r\ninvert_y_axis = 0\r\n",parsed)&&parsed.mode==4&&parsed.ignored,"whitespace and CRLF accepted like shim");
    check(std::string(vrsMode(4))=="Debug","Debug world VRS mode remains available");
    check(controlPath().empty(),"ordinary rig process cannot resolve a game-control write path");
    wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
    const auto dir=std::filesystem::path(temp)/(L"edup-beyond-controls-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const auto file=dir/L"foveation.cfg";
    DWORD error=0;Snapshot after;std::string why;
    check(publishAt(file.wstring(),{1,1,1},error),"complete file published in isolated fixture directory");
    check(writeFieldAt(file.wstring(),0,3,after,why)&&after.valid&&after.value.mode==3&&after.value.ignored==1&&after.value.invert==1,"mode edit preserves both unrelated gaze controls");
    check(writeFieldAt(file.wstring(),1,0,after,why)&&after.value.mode==3&&!after.value.ignored&&after.value.invert==1,"gaze edit preserves mode and invert");
    const auto before=readBytes(file);
    check(!publishAt(file.wstring(),{5,0,0},error)&&readBytes(file)==before,"invalid publication cannot overwrite known-good file");
    {std::ofstream f(file,std::ios::binary|std::ios::trunc);f<<"mode=4\n";}
    check(!writeFieldAt(file.wstring(),0,1,after,why)&&readBytes(file)=="mode=4\n","malformed existing controls are preserved rather than silently repaired");
    std::filesystem::remove(file);
    check(writeFieldAt(file.wstring(),2,1,after,why)&&after.valid&&!after.value.mode&&!after.value.ignored&&after.value.invert==1,"missing file initializes all three fields from OFF defaults");
    for(const auto& item:std::filesystem::directory_iterator(dir))check(item.path()==file,"successful replacement leaves no temp files");
    std::filesystem::remove_all(dir);
    if(argc>1&&std::string(argv[1])!="--self-test") {
        const auto root=std::filesystem::path(argv[1]);const auto menu=readBytes(root/L"src/d3d11/menu.cpp");
        const auto pages=readBytes(root/L"src/d3d11/menu_pages.h");
        const auto wired=[&](const std::string& s){return pages.find("\"VRS / Frame generation\", false, true, false, MenuPageSource::VrsFrameGeneration")!=std::string::npos&&s.find("case MenuPageSource::VrsFrameGeneration: addVrsFrameGenerationRows(p); break;")!=std::string::npos&&s.find("beyond_vrs::writeField(")!=std::string::npos&&s.find("WriteDestination::BeyondVrsPoll")!=std::string::npos&&s.find("nativeVrsStatus()")!=std::string::npos&&s.find("nativeFgAutoStatus()")!=std::string::npos;};
        check(wired(menu),"VRS/FG page exposes the real file writer and native status");
        auto mutant=menu;const auto at=mutant.find("beyond_vrs::writeField(");if(at!=std::string::npos)mutant.erase(at,std::string("beyond_vrs::writeField(").size());
        check(!wired(mutant),"negative control catches removing the actual VRS writer");
        const std::string tick="drainWrites();\n        tickBeyondControls(now);";
        check(menu.find(tick)!=std::string::npos,"VR menu tick polls real VRS controls after completed writes");
        check(menu.find("if(!s.open||s.pages.empty()||strcmp(s.pages[s.page].name,\"VRS / Frame generation\"))return;")!=std::string::npos,"closed or unrelated pages do no VRS poll/format work");
    }
    std::printf("Beyond controls: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
