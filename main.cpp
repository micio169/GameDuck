// GameDuck v1.2.0 - 게임 볼륨 즉시 낮추기
// v1.2.0 변경사항:
//   - UI 개선: 여백 정리, 창 크기 최적화 (960x760)
//   - 게임 목록 스크롤 버그 수정 (RefreshGameList가 스크롤 위치를 보존)
//   - 더 많은 게임 추가 (Minecraft, Roblox, Genshin Impact 등 26개, 총 36개)
//   - 게임 검색 필터, 등록 게임 개수 표시
//   - v1.2.0 릴리스 전 수정: 프로세스명 오류 4건 정정
//     (FIFA24.exe->FC24.exe, Hogwarts Legacy.exe->HogwartsLegacy.exe,
//      TEKKEN 8.exe->Polaris-Win64-Shipping.exe, T6SP.exe->cod.exe)
// 동작 원리: Windows Core Audio 세션 API로 게임 프로세스별 볼륨만 조절합니다.
// 게임에 DLL을 주입하지 않고, 게임 메모리를 읽지 않으며, 시스템 전체 볼륨은 건드리지 않습니다.
// 트리거 키/마우스 버튼은 전역 로우레벨 훅으로 "듣기만" 합니다. 입력을 가로채지 않으므로
// 게임 조작은 그대로 전달됩니다. (키 매핑 캡처 중 그 한 번의 입력만 제외)
// 백그라운드 부담: 훅은 이벤트 방식이라 폴링이 없고, 세션 스캔은 2초에 한 번만 합니다.
// 목표 메모리 사용량 15MB 미만, 네이티브 C++ 단일 exe, .NET/Electron 없음.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <psapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include "lang.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uxtheme.lib")

static std::wstring U8(const char* s){
    if(!s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if(n<=0) return L"";
    std::wstring w(n-1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}
static std::string W2U8(const std::wstring& ws){
    if(ws.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    if(n<=0) return "";
    std::string s(n-1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, NULL, NULL);
    return s;
}
static std::wstring ToLower(std::wstring s){ for(auto& c: s) c = towlower(c); return s; }
static std::wstring BaseName(const std::wstring& p){
    size_t pos = p.find_last_of(L"\\/");
    return (pos==std::wstring::npos) ? p : p.substr(pos+1);
}

// ---------- 설정 ----------
struct Binding{ int type; int vk; int mouse; int mods; }; // type 0=키보드 1=마우스, mouse 1좌 2우 3휠 4 X1 5 X2, mods bit0 Ctrl bit1 Alt bit2 Shift bit3 Win
struct Game{ std::wstring exe; std::wstring name; bool enabled; };
static bool g_enabled=true;
static int g_mode=0; // 0 Hold(누르고 있는 동안만) 1 Toggle(누를 때마다 전환)
static int g_duckLevel=10; // 낮춘 볼륨 % (절대 볼륨, 0~50)
static int g_fadeMs=0; // 0=즉시
static bool g_startWithWindows=false;
static bool g_minToTray=true;
static bool g_forceExit=false;  // ExitApp() sets this so WM_CLOSE really destroys the window
static bool g_allGames=false; // 목록 외 실행 중 게임/앱도 덕킹
static std::vector<Binding> g_bindings;
static std::vector<Game> g_games;
static wchar_t g_iniPath[MAX_PATH]=L"";
static wchar_t g_exePath[MAX_PATH]=L"";

static HWND g_hwnd=NULL;
static HFONT g_fontTitle=NULL, g_fontNormal=NULL, g_fontBold=NULL, g_fontSmall=NULL;
static HHOOK g_kbHook=NULL, g_mouseHook=NULL;
static bool g_capturing=false;
static DWORD g_captureStart=0;
static bool g_ducked=false;       // 현재 덕킹 중인가
static bool g_toggleState=false;  // 토글 모드 상태
static int g_holdCount=0;         // 홀드 모드에서 누르고 있는 바인딩 수
static std::map<DWORD,float> g_origVol; // pid -> 덕킹 전 원래 볼륨
static std::map<DWORD,std::wstring> g_duckedExe;
static float g_fadeProgress=1.0f; // 1=목표 도달
static bool g_fadeTargetDuck=false;
static DWORD g_fadeStartMs=0;
static bool g_fadeActive=false;
static std::wstring g_statusText;
static NOTIFYICONDATAW g_nid={0};

static HWND g_chkEnable=NULL, g_statusLabel=NULL, g_bindList=NULL, g_modeHold=NULL, g_modeToggle=NULL;
static HWND g_captureLabel=NULL, g_duckSlider=NULL, g_duckLabel=NULL, g_fadeSlider=NULL, g_fadeLabel=NULL;
static HWND g_volBar=NULL, g_volLabel=NULL, g_gameList=NULL, g_gameExeBox=NULL, g_memLabel=NULL, g_logLabel=NULL;
static HWND g_chkStartup=NULL, g_chkTray=NULL, g_chkAllGames=NULL;
static int g_lang=0;
static const LangStrings& L(){ return LANGS[g_lang]; }
static HWND g_langCombo=NULL, g_langLabel=NULL, g_grp1=NULL, g_grp2=NULL, g_grp3=NULL, g_titleLabel=NULL, g_subLabel=NULL;
// v1.2.0: 게임 검색 필터
static HWND g_gameSearchBox=NULL;
static std::wstring g_gameFilter;

#define WM_APP_DUCK_ON  (WM_APP+1)
#define WM_APP_DUCK_OFF (WM_APP+2)
#define WM_APP_CAPTURE_TIMEOUT (WM_APP+3)
#define WM_TRAYICON (WM_APP+10)
#define ID_TRAY_OPEN 4001
#define ID_TRAY_TOGGLE 4002
#define ID_TRAY_EXIT 4003
#define TIMER_REFRESH 1
#define TIMER_FADE 2
#define TIMER_CAPTURE 3

// ---------- 기본 게임 목록 (프로세스명 기준, 여러 개 동시 지원) ----------
// v1.2.0: 36개로 확대 (기본 10개 + 신규 26개)
static void SetDefaultGames(){
    g_games.clear();
    auto add=[&](const char* exe, const char* name){ Game g; g.exe=U8(exe); g.name=U8(name); g.enabled=true; g_games.push_back(g); };
    // FPS / TPS
    add("VALORANT-Win64-Shipping.exe","Valorant");
    add("League of Legends.exe","League of Legends");
    add("cs2.exe","CS2");
    add("TslGame.exe","PUBG");
    add("Overwatch.exe","Overwatch 2");
    add("r5apex.exe","Apex Legends");
    add("FortniteClient-Win64-Shipping.exe","Fortnite");
    add("EternalReturn.exe","Eternal Return");
    add("EscapeFromTarkov.exe","Escape from Tarkov");
    add("FO4Client.exe","FC Online");
    // v1.2.0 신규 추가
    add("Minecraft.Windows.exe","Minecraft (Bedrock)");
    add("javaw.exe","Minecraft (Java)");
    add("RobloxPlayerBeta.exe","Roblox");
    add("GenshinImpact.exe","Genshin Impact");
    add("YuanShen.exe","Genshin Impact (CN)");
    add("StarRail.exe","Honkai: Star Rail");
    add("ZenlessZoneZero.exe","Zenless Zone Zero");
    add("LOSTARK.exe","Lost Ark");
    add("Diablo IV.exe","Diablo IV");
    add("destiny2.exe","Destiny 2");
    add("RainbowSix.exe","Rainbow Six Siege");
    add("HellLetLoose-Win64-Shipping.exe","Hell Let Loose");
    add("eldenring.exe","Elden Ring");
    add("DarkSoulsIII.exe","Dark Souls III");
    add("Palworld-Win64-Shipping.exe","Palworld");
    add("Cyberpunk2077.exe","Cyberpunk 2077");
    add("HogwartsLegacy.exe","Hogwarts Legacy");
    add("Polaris-Win64-Shipping.exe","Tekken 8");
    add("FC24.exe","EA Sports FC 24");
    add("dota2.exe","Dota 2");
    add("deadlock.exe","Deadlock");
    add("pathofexile_x64.exe","Path of Exile");
    add("cod.exe","Call of Duty: Black Ops 6");
    add("MarvelRivals-Win64-Shipping.exe","Marvel Rivals");
    add("DNF.exe","Dungeon & Fighter");
    add("MapleStory.exe","MapleStory");
}
static void SetDefaultBindings(){
    g_bindings.clear();
    Binding b; b.type=1; b.vk=0; b.mouse=4; b.mods=0; // 마우스 X1 (옆 버튼)
    g_bindings.push_back(b);
}

// ---------- INI (직접 파서, exe 옆 GameDuck.ini) ----------
static std::wstring IniPath(){
    std::wstring p = g_exePath;
    size_t pos = p.find_last_of(L"\\/");
    std::wstring dir = (pos==std::wstring::npos) ? L"" : p.substr(0,pos+1);
    return dir + L"GameDuck.ini";
}
static void SaveSettings(){
    std::string out;
    char buf[512];
    snprintf(buf,sizeof(buf),"Enabled=%d\nMode=%d\nDuckLevel=%d\nFadeMs=%d\nStartWithWindows=%d\nMinimizeToTray=%d\nAllGames=%d\nLang=%d\n",
        g_enabled?1:0, g_mode, g_duckLevel, g_fadeMs, g_startWithWindows?1:0, g_minToTray?1:0, g_allGames?1:0, g_lang);
    out += buf;
    snprintf(buf,sizeof(buf),"BindingCount=%d\n",(int)g_bindings.size()); out += buf;
    for(size_t i=0;i<g_bindings.size();i++){
        snprintf(buf,sizeof(buf),"Binding%zu=%d,%d,%d,%d\n",i,g_bindings[i].type,g_bindings[i].vk,g_bindings[i].mouse,g_bindings[i].mods);
        out += buf;
    }
    snprintf(buf,sizeof(buf),"GameCount=%d\n",(int)g_games.size()); out += buf;
    for(size_t i=0;i<g_games.size();i++){
        out += "Game" + std::to_string(i) + "=" + W2U8(g_games[i].exe) + "|" + W2U8(g_games[i].name) + "|" + (g_games[i].enabled?"1":"0") + "\n";
    }
    std::wstring path = IniPath();
    FILE* f = _wfopen(path.c_str(), L"wb");
    if(f){ fwrite(out.c_str(),1,out.size(),f); fclose(f); }
}
static int IniInt(const std::map<std::string,std::string>& m, const char* k, int def){
    auto it=m.find(k); if(it==m.end()) return def; return atoi(it->second.c_str());
}
static void LoadSettings(){
    SetDefaultGames(); SetDefaultBindings();
    std::wstring path = IniPath();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if(!f) return;
    std::map<std::string,std::string> m;
    char line[1024];
    while(fgets(line,sizeof(line),f)){
        std::string s=line; while(!s.empty()&&(s.back()=='\n'||s.back()=='\r')) s.pop_back();
        size_t eq=s.find('='); if(eq==std::string::npos) continue;
        m[s.substr(0,eq)]=s.substr(eq+1);
    }
    fclose(f);
    g_enabled = IniInt(m,"Enabled",1)!=0;
    g_mode = IniInt(m,"Mode",0);
    g_duckLevel = IniInt(m,"DuckLevel",10); if(g_duckLevel<0) g_duckLevel=0; if(g_duckLevel>50) g_duckLevel=50;
    g_fadeMs = IniInt(m,"FadeMs",0); if(g_fadeMs<0) g_fadeMs=0; if(g_fadeMs>300) g_fadeMs=300;
    g_startWithWindows = IniInt(m,"StartWithWindows",0)!=0;
    g_minToTray = IniInt(m,"MinimizeToTray",1)!=0;
    g_allGames = IniInt(m,"AllGames",0)!=0;
    g_lang = IniInt(m,"Lang",0); if(g_lang<0) g_lang=0; if(g_lang>=LANG_COUNT) g_lang=0;
    int bc = IniInt(m,"BindingCount",-1);
    if(bc>=0){ g_bindings.clear(); for(int i=0;i<bc;i++){ char k[32]; snprintf(k,sizeof(k),"Binding%d",i); auto it=m.find(k); if(it==m.end()) continue; int t=0,v=0,mo=0,md=0; if(sscanf(it->second.c_str(),"%d,%d,%d,%d",&t,&v,&mo,&md)==4){ Binding b; b.type=t; b.vk=v; b.mouse=mo; b.mods=md; g_bindings.push_back(b);} } }
    int gc = IniInt(m,"GameCount",-1);
    if(gc>=0){ g_games.clear(); for(int i=0;i<gc;i++){ char k[32]; snprintf(k,sizeof(k),"Game%d",i); auto it=m.find(k); if(it==m.end()) continue; std::string s=it->second; size_t p1=s.find('|'); size_t p2=s.rfind('|'); if(p1==std::string::npos||p2==std::string::npos||p2<=p1) continue; Game g; g.exe=U8(s.substr(0,p1).c_str()); g.name=U8(s.substr(p1+1,p2-p1-1).c_str()); g.enabled=(s.substr(p2+1)=="1"); g_games.push_back(g);} }
    if(g_games.empty()) SetDefaultGames();
    if(g_bindings.empty()) SetDefaultBindings();
}

// ---------- 키 이름 ----------
static std::wstring VkName(int vk){
    if(vk==0) return (g_lang==0) ? U8("없음") : U8("None");
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    wchar_t buf[64]=L"";
    // 확장 키 처리 (방향키 등)
    LONG lparam = (LONG)(sc<<16);
    if(vk==VK_LEFT||vk==VK_RIGHT||vk==VK_UP||vk==VK_DOWN||vk==VK_HOME||vk==VK_END||vk==VK_PRIOR||vk==VK_NEXT||vk==VK_INSERT||vk==VK_DELETE||vk==VK_NUMLOCK) lparam |= (1<<24);
    if(GetKeyNameTextW(lparam, buf, 64)>0) return buf;
    wchar_t tmp[32]; swprintf(tmp,32,L"VK %d",vk); return tmp;
}
static std::wstring MouseName(int m){
    if(g_lang==0){
        switch(m){ case 1: return U8("마우스 왼쪽"); case 2: return U8("마우스 오른쪽"); case 3: return U8("마우스 휠 클릭"); case 4: return U8("마우스 X1 (옆 버튼)"); case 5: return U8("마우스 X2 (옆 버튼)"); }
        return U8("마우스");
    }
    switch(m){ case 1: return U8("Mouse Left"); case 2: return U8("Mouse Right"); case 3: return U8("Mouse Wheel Click"); case 4: return U8("Mouse X1 (side button)"); case 5: return U8("Mouse X2 (side button)"); }
    return U8("Mouse");
}
static std::wstring BindingLabel(const Binding& b){
    std::wstring s;
    if(b.mods&1) s+=U8("Ctrl+");
    if(b.mods&2) s+=U8("Alt+");
    if(b.mods&4) s+=U8("Shift+");
    if(b.mods&8) s+=U8("Win+");
    s += (b.type==1) ? MouseName(b.mouse) : VkName(b.vk);
    return s;
}
static int CurrentMods(){
    int m=0;
    if(GetAsyncKeyState(VK_CONTROL)&0x8000) m|=1;
    if(GetAsyncKeyState(VK_MENU)&0x8000) m|=2;
    if(GetAsyncKeyState(VK_SHIFT)&0x8000) m|=4;
    if(GetAsyncKeyState(VK_LWIN)&0x8000||GetAsyncKeyState(VK_RWIN)&0x8000) m|=8;
    return m;
}
static bool ModsMatch(int want){
    return (CurrentMods() & want) == want;
}

// ---------- 오디오 세션 덕킹 (Core Audio, 게임별 볼륨만) ----------
static std::wstring ExeNameFromPid(DWORD pid){
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if(!h) return L"";
    wchar_t buf[MAX_PATH]=L"";
    DWORD sz=MAX_PATH;
    if(QueryFullProcessImageNameW(h, 0, buf, &sz)){
        CloseHandle(h); return BaseName(buf);
    }
    if(GetModuleFileNameExW(h, NULL, buf, MAX_PATH)){
        CloseHandle(h); return BaseName(buf);
    }
    CloseHandle(h); return L"";
}
static bool IsTargetExe(const std::wstring& exe){
    if(exe.empty()) return false;
    std::wstring low = ToLower(exe);
    if(g_allGames){
        if(low==L"system sounds"||low==L"audiodg.exe") return false;
        return true;
    }
    for(auto& g: g_games) if(g.enabled && ToLower(g.exe)==low) return true;
    return false;
}
template<typename F>
static void ForEachTargetSession(F&& fn){
    IMMDeviceEnumerator* pEnum=NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if(FAILED(hr)||!pEnum) return;
    IMMDevice* pDev=NULL;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
    if(FAILED(hr)||!pDev){ pEnum->Release(); return; }
    IAudioSessionManager2* pMgr=NULL;
    hr = pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pMgr);
    if(FAILED(hr)||!pMgr){ pDev->Release(); pEnum->Release(); return; }
    IAudioSessionEnumerator* pSessEnum=NULL;
    hr = pMgr->GetSessionEnumerator(&pSessEnum);
    if(SUCCEEDED(hr)&&pSessEnum){
        int count=0; pSessEnum->GetCount(&count);
        for(int i=0;i<count;i++){
            IAudioSessionControl* pCtl=NULL;
            if(FAILED(pSessEnum->GetSession(i,&pCtl))||!pCtl) continue;
            AudioSessionState st; pCtl->GetState(&st);
            if(st==AudioSessionStateExpired){ pCtl->Release(); continue; }
            IAudioSessionControl2* pCtl2=NULL;
            if(SUCCEEDED(pCtl->QueryInterface(__uuidof(IAudioSessionControl2),(void**)&pCtl2))&&pCtl2){
                DWORD pid=0; pCtl2->GetProcessId(&pid);
                std::wstring exe = (pid!=0) ? ExeNameFromPid(pid) : L"";
                ISimpleAudioVolume* pVol=NULL;
                if(SUCCEEDED(pCtl->QueryInterface(__uuidof(ISimpleAudioVolume),(void**)&pVol))&&pVol){
                    fn(pid, exe, pVol);
                    pVol->Release();
                }
                pCtl2->Release();
            }
            pCtl->Release();
        }
        pSessEnum->Release();
    }
    pMgr->Release(); pDev->Release(); pEnum->Release();
}
static void ApplyVolumeForDuckState(float progress){
    float target = g_duckLevel/100.0f;
    ForEachTargetSession([&](DWORD pid, const std::wstring& exe, ISimpleAudioVolume* pVol){
        auto it = g_origVol.find(pid);
        if(it==g_origVol.end()){
            float cur=1.0f; pVol->GetMasterVolume(&cur);
            g_origVol[pid]=cur; g_duckedExe[pid]=exe;
            it = g_origVol.find(pid);
        }
        float orig = it->second;
        float v = orig + (target-orig)*progress;
        if(v<0) v=0;
        if(v>1) v=1;
        pVol->SetMasterVolume(v, NULL);
    });
}
static void RestoreAllVolumes(){
    float target = 0; (void)target;
    ForEachTargetSession([&](DWORD pid, const std::wstring&, ISimpleAudioVolume* pVol){
        auto it = g_origVol.find(pid);
        if(it!=g_origVol.end()) pVol->SetMasterVolume(it->second, NULL);
    });
    g_origVol.clear(); g_duckedExe.clear();
    g_fadeActive=false; g_fadeProgress=1.0f;
}
static void StartDuck(bool on){
    if(on==g_ducked && !g_fadeActive) return;
    g_ducked = on;
    g_fadeTargetDuck = on;
    if(g_fadeMs<=0){
        g_fadeProgress=1.0f; g_fadeActive=false;
        if(on) ApplyVolumeForDuckState(1.0f);
        else RestoreAllVolumes();
    } else {
        g_fadeStartMs = GetTickCount();
        g_fadeProgress = on ? 0.0f : 1.0f;
        g_fadeActive=true;
        SetTimer(g_hwnd, TIMER_FADE, 15, NULL);
        ApplyVolumeForDuckState(g_fadeProgress);
    }
    if(g_volBar){
        int pct = on ? g_duckLevel : 100;
        SendMessageW(g_volBar, PBM_SETPOS, (WPARAM)pct, 0);
    }
}
static void SetStatus(const std::wstring& s){
    g_statusText=s;
    if(g_statusLabel) SetWindowTextW(g_statusLabel, s.c_str());
    if(g_logLabel) SetWindowTextW(g_logLabel, s.c_str());
}

// ---------- 전역 훅 (듣기만 함 — 입력 가로채지 않음) ----------
static bool BindingMatchesKey(const Binding& b, int vk){
    if(b.type!=0) return false;
    if(b.vk!=vk) return false;
    if(vk==VK_CONTROL||vk==VK_MENU||vk==VK_SHIFT||vk==VK_LWIN||vk==VK_RWIN||vk==VK_LCONTROL||vk==VK_RCONTROL||vk==VK_LMENU||vk==VK_RMENU||vk==VK_LSHIFT||vk==VK_RSHIFT) return true;
    return ModsMatch(b.mods);
}
static bool BindingMatchesMouse(const Binding& b, int btn){
    if(b.type!=1) return false;
    if(b.mouse!=btn) return false;
    return ModsMatch(b.mods);
}
static bool AnyBindingMatchesKey(int vk){
    for(auto& b: g_bindings) if(BindingMatchesKey(b,vk)) return true;
    return false;
}
static bool AnyBindingMatchesMouse(int btn){
    for(auto& b: g_bindings) if(BindingMatchesMouse(b,btn)) return true;
    return false;
}
static void OnTriggerDown(){
    if(!g_enabled || g_hwnd==NULL) return;
    if(g_mode==0){ // Hold
        g_holdCount++;
        if(g_holdCount==1) PostMessageW(g_hwnd, WM_APP_DUCK_ON, 0, 0);
    } else { // Toggle
        g_toggleState=!g_toggleState;
        PostMessageW(g_hwnd, g_toggleState?WM_APP_DUCK_ON:WM_APP_DUCK_OFF, 0, 0);
    }
}
static void OnTriggerUp(){
    if(g_mode!=0) return;
    if(g_holdCount>0) g_holdCount--;
    if(g_holdCount==0) PostMessageW(g_hwnd, WM_APP_DUCK_OFF, 0, 0);
}
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam){
    if(nCode==HC_ACTION){
        KBDLLHOOKSTRUCT* k=(KBDLLHOOKSTRUCT*)lParam;
        bool down = (wParam==WM_KEYDOWN||wParam==WM_SYSKEYDOWN);
        bool up = (wParam==WM_KEYUP||wParam==WM_SYSKEYUP);
        if(g_capturing && down){
            int vk=(int)k->vkCode;
            if(vk==VK_ESCAPE){
                g_capturing=false; KillTimer(g_hwnd, TIMER_CAPTURE);
                SetStatus(U8(L().msgCancelled));
                if(g_captureLabel) SetWindowTextW(g_captureLabel, U8(L().msgCancelled).c_str());
            } else {
                Binding b; b.type=0; b.vk=vk; b.mouse=0;
                int mods=CurrentMods();
                if(vk==VK_CONTROL||vk==VK_LCONTROL||vk==VK_RCONTROL) mods&=~1;
                if(vk==VK_MENU||vk==VK_LMENU||vk==VK_RMENU) mods&=~2;
                if(vk==VK_SHIFT||vk==VK_LSHIFT||vk==VK_RSHIFT) mods&=~4;
                b.mods=mods;
                g_bindings.push_back(b);
                g_capturing=false; KillTimer(g_hwnd, TIMER_CAPTURE);
                SaveSettings();
                PostMessageW(g_hwnd, WM_APP+20, 0, 0);
                SetStatus(U8(L().msgAdded) + BindingLabel(b));
            }
            return 1;
        }
        if(!g_capturing && g_enabled){
            extern std::map<int,bool> g_keyDownMap;
            if(down){
                if(!g_keyDownMap[(int)k->vkCode]){
                    g_keyDownMap[(int)k->vkCode]=true;
                    if(AnyBindingMatchesKey((int)k->vkCode)) OnTriggerDown();
                }
            } else if(up){
                g_keyDownMap[(int)k->vkCode]=false;
                if(AnyBindingMatchesKey((int)k->vkCode)) OnTriggerUp();
            }
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}
std::map<int,bool> g_keyDownMap;
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam){
    if(nCode==HC_ACTION){
        MSLLHOOKSTRUCT* m=(MSLLHOOKSTRUCT*)lParam;
        int btn=0; bool down=false, up=false;
        if(wParam==WM_LBUTTONDOWN){ btn=1; down=true; }
        else if(wParam==WM_LBUTTONUP){ btn=1; up=true; }
        else if(wParam==WM_RBUTTONDOWN){ btn=2; down=true; }
        else if(wParam==WM_RBUTTONUP){ btn=2; up=true; }
        else if(wParam==WM_MBUTTONDOWN){ btn=3; down=true; }
        else if(wParam==WM_MBUTTONUP){ btn=3; up=true; }
        else if(wParam==WM_XBUTTONDOWN){ btn = (HIWORD(m->mouseData)==XBUTTON1)?4:5; down=true; }
        else if(wParam==WM_XBUTTONUP){ btn = (HIWORD(m->mouseData)==XBUTTON1)?4:5; up=true; }
        if(btn!=0){
            if(g_capturing && down){
                Binding b; b.type=1; b.vk=0; b.mouse=btn; b.mods=CurrentMods();
                g_bindings.push_back(b);
                g_capturing=false; KillTimer(g_hwnd, TIMER_CAPTURE);
                SaveSettings();
                PostMessageW(g_hwnd, WM_APP+20, 0, 0);
                SetStatus(U8(L().msgAdded) + BindingLabel(b));
                return 1;
            }
            if(!g_capturing && g_enabled){
                if(down && AnyBindingMatchesMouse(btn)) OnTriggerDown();
                else if(up && AnyBindingMatchesMouse(btn)) OnTriggerUp();
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}
static void InstallHooks(){
    if(!g_kbHook) g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(NULL), 0);
    if(!g_mouseHook) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(NULL), 0);
}
static void UninstallHooks(){
    if(g_kbHook){ UnhookWindowsHookEx(g_kbHook); g_kbHook=NULL; }
    if(g_mouseHook){ UnhookWindowsHookEx(g_mouseHook); g_mouseHook=NULL; }
}

// ---------- 트레이 / 시작 프로그램 ----------
static void AddTray(){
    g_nid.cbSize=sizeof(g_nid); g_nid.hWnd=g_hwnd; g_nid.uID=1;
    g_nid.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP; g_nid.uCallbackMessage=WM_TRAYICON;
    g_nid.hIcon=(HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, 0);
    if(!g_nid.hIcon) g_nid.hIcon=LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, (g_lang==0?U8("GameDuck — 게임 볼륨 덕커"):U8("GameDuck — Game Volume Ducker")).c_str());
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void RemoveTray(){ Shell_NotifyIconW(NIM_DELETE, &g_nid); }
static void ApplyStartup(){
    HKEY hKey=NULL;
    if(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey)==ERROR_SUCCESS){
        if(g_startWithWindows){
            std::wstring v = L"\"" + std::wstring(g_exePath) + L"\"";
            RegSetValueExW(hKey, L"GameDuck", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size()+1)*sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"GameDuck");
        }
        RegCloseKey(hKey);
    }
}
static double CurrentMemoryMB(){
    PROCESS_MEMORY_COUNTERS pmc={0}; pmc.cb=sizeof(pmc);
    if(GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.WorkingSetSize/1024.0/1024.0;
    return 0;
}

// ---------- v1.2.0: 게임 목록 갱신 (스크롤 위치 보존) ----------
static void RefreshGameList(){
    if(!g_gameList) return;

    // ★ v1.2.0 핵심 수정: 스크롤 위치를 보존한다
    // 먼저 현재 스크롤 위치와 선택 상태를 저장
    int topIndex = ListView_GetTopIndex(g_gameList);
    int selIndex = ListView_GetNextItem(g_gameList, -1, LVNI_SELECTED);

    // 실행 중 프로세스 집합 (상태 표시용)
    std::map<std::wstring,bool> running;
    ForEachTargetSession([&](DWORD, const std::wstring& exe, ISimpleAudioVolume* pVol){
        float v=0; pVol->GetMasterVolume(&v);
        running[ToLower(exe)]=true;
    });
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap!=INVALID_HANDLE_VALUE){
        PROCESSENTRY32W pe; pe.dwSize=sizeof(pe);
        if(Process32FirstW(snap,&pe)){ do{ running[ToLower(pe.szExeFile)]=true; }while(Process32NextW(snap,&pe)); }
        CloseHandle(snap);
    }

    // 필터링된 게임 목록 구성
    std::vector<int> filteredIndices;
    for(size_t i=0;i<g_games.size();i++){
        if(!g_gameFilter.empty()){
            std::wstring nameLow = ToLower(g_games[i].name);
            std::wstring exeLow = ToLower(g_games[i].exe);
            std::wstring filterLow = ToLower(g_gameFilter);
            if(nameLow.find(filterLow)==std::wstring::npos && exeLow.find(filterLow)==std::wstring::npos)
                continue;
        }
        filteredIndices.push_back((int)i);
    }

    int currentCount = ListView_GetItemCount(g_gameList);
    int targetCount = (int)filteredIndices.size();

    // 아이템 수를 맞춘다 (삭제/추가를 최소화)
    // 먼저 기존 아이템들의 체크 상태 변경 알림을 무시하기 위해 직접 텍스트만 갱신
    // WM_SETREDRAW로 갱신 중 깜빡임 방지
    SendMessageW(g_gameList, WM_SETREDRAW, FALSE, 0);

    // 초과 아이템 삭제
    while(currentCount > targetCount){
        ListView_DeleteItem(g_gameList, currentCount-1);
        currentCount--;
    }

    for(int fi=0; fi<targetCount; fi++){
        int gi = filteredIndices[fi];
        bool run = running.count(ToLower(g_games[gi].exe))>0;
        std::wstring st = run ? U8(L().runState) : U8(L().notRun);
        if(run && g_ducked) st += U8(" · ") + U8(L().duckingShort);

        if(fi < currentCount){
            // 기존 아이템 업데이트 (삽입/삭제 없이 텍스트만 변경)
            ListView_SetItemText(g_gameList, fi, 0, (LPWSTR)g_games[gi].name.c_str());
            ListView_SetItemText(g_gameList, fi, 1, (LPWSTR)g_games[gi].exe.c_str());
            ListView_SetItemText(g_gameList, fi, 2, (LPWSTR)st.c_str());
            // 체크 상태도 변경이 필요한 경우에만 설정
            BOOL curCheck = ListView_GetCheckState(g_gameList, fi);
            BOOL wantCheck = g_games[gi].enabled ? TRUE : FALSE;
            if(curCheck != wantCheck){
                ListView_SetCheckState(g_gameList, fi, wantCheck);
            }
            // lParam에 원래 인덱스 저장 (체크 변경 처리용)
            LVITEMW lvi={0}; lvi.mask=LVIF_PARAM; lvi.iItem=fi; lvi.lParam=(LPARAM)gi;
            ListView_SetItem(g_gameList, &lvi);
        } else {
            // 새 아이템 추가
            LVITEMW item={0}; item.mask=LVIF_TEXT|LVIF_PARAM; item.iItem=fi; item.iSubItem=0;
            item.pszText=(LPWSTR)g_games[gi].name.c_str();
            item.lParam=(LPARAM)gi;
            int idx=ListView_InsertItem(g_gameList,&item);
            ListView_SetItemText(g_gameList, idx, 1, (LPWSTR)g_games[gi].exe.c_str());
            ListView_SetItemText(g_gameList, idx, 2, (LPWSTR)st.c_str());
            ListView_SetCheckState(g_gameList, idx, g_games[gi].enabled?TRUE:FALSE);
        }
    }

    SendMessageW(g_gameList, WM_SETREDRAW, TRUE, 0);

    // ★ 스크롤 위치 복원
    if(topIndex > 0 && topIndex < targetCount){
        ListView_EnsureVisible(g_gameList, targetCount-1, FALSE);
        ListView_EnsureVisible(g_gameList, topIndex, FALSE);
    }
    if(selIndex >= 0 && selIndex < targetCount){
        ListView_SetItemState(g_gameList, selIndex, LVIS_SELECTED|LVIS_FOCUSED, LVIS_SELECTED|LVIS_FOCUSED);
    }

    InvalidateRect(g_gameList, NULL, TRUE);
}

static void RefreshBindingList(){
    if(!g_bindList) return;
    SendMessageW(g_bindList, LB_RESETCONTENT, 0, 0);
    for(auto& b: g_bindings){
        std::wstring s = BindingLabel(b);
        SendMessageW(g_bindList, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    if(g_bindings.empty()){
        std::wstring emptyMsg = (g_lang==0) ? U8("(매핑 없음 — [키/마우스 추가]를 눌러 등록하세요)")
            : U8("(No mappings — press [") + U8(L().addBind) + U8("] to register)");
        SendMessageW(g_bindList, LB_ADDSTRING, 0, (LPARAM)emptyMsg.c_str());
    }
}
static void RefreshAllUI(){
    RefreshBindingList(); RefreshGameList();
    if(g_chkEnable) SendMessageW(g_chkEnable, BM_SETCHECK, g_enabled?BST_CHECKED:BST_UNCHECKED, 0);
    if(g_modeHold) SendMessageW(g_modeHold, BM_SETCHECK, g_mode==0?BST_CHECKED:BST_UNCHECKED, 0);
    if(g_modeToggle) SendMessageW(g_modeToggle, BM_SETCHECK, g_mode==1?BST_CHECKED:BST_UNCHECKED, 0);
    if(g_duckSlider) SendMessageW(g_duckSlider, TBM_SETPOS, TRUE, g_duckLevel);
    if(g_fadeSlider) SendMessageW(g_fadeSlider, TBM_SETPOS, TRUE, g_fadeMs);
    if(g_chkStartup) SendMessageW(g_chkStartup, BM_SETCHECK, g_startWithWindows?BST_CHECKED:BST_UNCHECKED, 0);
    if(g_chkTray) SendMessageW(g_chkTray, BM_SETCHECK, g_minToTray?BST_CHECKED:BST_UNCHECKED, 0);
    if(g_chkAllGames) SendMessageW(g_chkAllGames, BM_SETCHECK, g_allGames?BST_CHECKED:BST_UNCHECKED, 0);
    wchar_t buf[128];
    swprintf(buf,128,U8(L().duckFmt).c_str(), g_duckLevel, g_duckLevel);
    if(g_duckLabel) SetWindowTextW(g_duckLabel, buf);
    swprintf(buf,128,U8(L().fadeFmt).c_str(), g_fadeMs, g_fadeMs==0?U8(L().fadeInstant).c_str():U8(L().fadeSmooth).c_str());
    if(g_fadeLabel) SetWindowTextW(g_fadeLabel, buf);
    if(g_volBar) SendMessageW(g_volBar, PBM_SETPOS, (WPARAM)(g_ducked?g_duckLevel:100), 0);
    swprintf(buf,128,U8(L().volFmt).c_str(), g_ducked?g_duckLevel:100);
    if(g_volLabel) SetWindowTextW(g_volLabel, buf);
}

// ---------- UI ----------
static HFONT MakeFont(int size, bool bold){
    std::wstring fname = U8(LANG_FONTS[g_lang]);
    return CreateFontW(-MulDiv(size, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72), 0,0,0,
        bold?FW_BOLD:FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, fname.c_str());
}
static HWND MakeLabel(HWND parent, int x, int y, int w, int ht, const wchar_t* text, HFONT font, bool center=false){
    HWND ctl = CreateWindowW(L"STATIC", text, WS_CHILD|WS_VISIBLE|(center?SS_CENTER:SS_LEFT), x,y,w,ht, parent, NULL, GetModuleHandleW(NULL), NULL);
    if(font) SendMessageW(ctl, WM_SETFONT, (WPARAM)font, TRUE);
    return ctl;
}
static HWND MakeButton(HWND parent, int x, int y, int w, int ht, const wchar_t* text, int id){
    HWND ctl = CreateWindowW(L"BUTTON", text, WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, x,y,w,ht, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    return ctl;
}
static HWND MakeCheck(HWND parent, int x, int y, int w, int ht, const wchar_t* text, int id){
    HWND ctl = CreateWindowW(L"BUTTON", text, WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, x,y,w,ht, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    return ctl;
}
enum { ID_ADD_BIND=101, ID_DEL_BIND, ID_CLEAR_BIND, ID_MODE_HOLD, ID_MODE_TOGGLE,
       ID_TEST_DUCK, ID_RESTORE_VOL, ID_ADD_GAME, ID_DEL_GAME, ID_REFRESH_GAME,
       ID_TRAY_BTN, ID_EXIT_BTN, ID_CHK_ENABLE, ID_CHK_STARTUP, ID_CHK_TRAY, ID_CHK_ALLGAMES, ID_LANG_COMBO,
       ID_GAME_SEARCH };

static void CreateUI(HWND hwnd){
    g_fontTitle = MakeFont(18, true);
    g_fontBold = MakeFont(10, true);
    g_fontNormal = MakeFont(10, false);
    g_fontSmall = MakeFont(9, false);
    HINSTANCE hi = GetModuleHandleW(NULL);

    // ── 헤더 ──
    g_titleLabel = MakeLabel(hwnd, 24, 14, 400, 30, U8("🦆 GameDuck v1.2.0").c_str(), g_fontTitle);
    g_subLabel = MakeLabel(hwnd, 24, 46, 640, 20, U8(L().subtitle).c_str(), g_fontSmall);
    g_chkEnable = MakeCheck(hwnd, 680, 16, 200, 26, U8(L().enable).c_str(), ID_CHK_ENABLE);
    SendMessageW(g_chkEnable, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    g_statusLabel = MakeLabel(hwnd, 680, 44, 240, 20, U8(L().ready).c_str(), g_fontBold);

    // ── 1. 트리거 ──
    g_grp1 = CreateWindowW(L"BUTTON", U8(L().grp1).c_str(), WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 16, 74, 920, 164, hwnd, NULL, hi, NULL);
    MakeLabel(hwnd, 32, 98, 300, 20, U8(L().bindLabel).c_str(), g_fontBold);
    g_bindList = CreateWindowW(L"LISTBOX", NULL, WS_CHILD|WS_VISIBLE|WS_BORDER|LBS_NOTIFY|WS_VSCROLL, 32, 120, 320, 100, hwnd, NULL, hi, NULL);
    SendMessageW(g_bindList, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    MakeButton(hwnd, 368, 120, 155, 28, U8(L().addBind).c_str(), ID_ADD_BIND);
    MakeButton(hwnd, 368, 152, 155, 28, U8(L().delBind).c_str(), ID_DEL_BIND);
    MakeButton(hwnd, 368, 184, 155, 28, U8(L().clearBind).c_str(), ID_CLEAR_BIND);
    g_captureLabel = MakeLabel(hwnd, 540, 120, 380, 40, U8(L().captureHint).c_str(), g_fontSmall);
    g_modeHold = MakeButton(hwnd, 540, 166, 180, 28, U8(L().holdOn).c_str(), ID_MODE_HOLD);
    g_modeToggle = MakeButton(hwnd, 726, 166, 180, 28, U8(L().toggleOff).c_str(), ID_MODE_TOGGLE);
    MakeLabel(hwnd, 540, 200, 380, 20, U8(L().noIntercept).c_str(), g_fontSmall);

    // ── 2. 볼륨 ──
    g_grp2 = CreateWindowW(L"BUTTON", U8(L().grp2).c_str(), WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 16, 248, 920, 128, hwnd, NULL, hi, NULL);
    g_duckLabel = MakeLabel(hwnd, 32, 272, 340, 20, U8("").c_str(), g_fontBold);
    g_duckSlider = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS, 32, 294, 320, 32, hwnd, NULL, hi, NULL);
    SendMessageW(g_duckSlider, TBM_SETRANGE, TRUE, MAKELONG(0,50));
    SendMessageW(g_duckSlider, TBM_SETPAGESIZE, 0, 5);
    g_fadeLabel = MakeLabel(hwnd, 380, 272, 200, 20, U8("").c_str(), g_fontBold);
    g_fadeSlider = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS, 380, 294, 200, 32, hwnd, NULL, hi, NULL);
    SendMessageW(g_fadeSlider, TBM_SETRANGE, TRUE, MAKELONG(0,300));
    SendMessageW(g_fadeSlider, TBM_SETPAGESIZE, 0, 50);
    MakeButton(hwnd, 620, 272, 130, 28, U8(L().testDuck).c_str(), ID_TEST_DUCK);
    MakeButton(hwnd, 760, 272, 130, 28, U8(L().restoreVol).c_str(), ID_RESTORE_VOL);
    g_volLabel = MakeLabel(hwnd, 620, 308, 268, 20, U8("").c_str(), g_fontNormal);
    g_volBar = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD|WS_VISIBLE|PBS_SMOOTH, 620, 332, 268, 18, hwnd, NULL, hi, NULL);
    SendMessageW(g_volBar, PBM_SETRANGE, 0, MAKELONG(0,100));
    SendMessageW(g_volBar, PBM_SETPOS, 100, 0);
    MakeLabel(hwnd, 32, 338, 560, 20, U8(L().duckNote).c_str(), g_fontSmall);

    // ── 3. 게임 목록 (확대) ──
    g_grp3 = CreateWindowW(L"BUTTON", U8(L().grp3).c_str(), WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 16, 386, 920, 240, hwnd, NULL, hi, NULL);

    // 게임 검색 필터 (v1.2.0 신규)
    std::wstring searchHint = (g_lang==0) ? U8("🔍 게임 검색...") : U8("🔍 Search games...");
    g_gameSearchBox = CreateWindowW(L"EDIT", NULL, WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL, 32, 410, 300, 24, hwnd, (HMENU)(INT_PTR)ID_GAME_SEARCH, hi, NULL);
    SendMessageW(g_gameSearchBox, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    SendMessageW(g_gameSearchBox, EM_SETCUEBANNER, TRUE, (LPARAM)searchHint.c_str());

    g_gameList = CreateWindowW(WC_LISTVIEWW, NULL, WS_CHILD|WS_VISIBLE|WS_BORDER|LVS_REPORT|LVS_SINGLESEL, 32, 440, 620, 170, hwnd, NULL, hi, NULL);
    SendMessageW(g_gameList, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    ListView_SetExtendedListViewStyle(g_gameList, LVS_EX_FULLROWSELECT|LVS_EX_CHECKBOXES|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col={0}; col.mask=LVCF_TEXT|LVCF_WIDTH;
    static std::wstring colGame = U8(L().colGame), colProc = U8(L().colProc), colState = U8(L().colState);
    col.pszText=(LPWSTR)colGame.c_str(); col.cx=200; ListView_InsertColumn(g_gameList,0,&col);
    col.pszText=(LPWSTR)colProc.c_str(); col.cx=260; ListView_InsertColumn(g_gameList,1,&col);
    col.pszText=(LPWSTR)colState.c_str(); col.cx=140; ListView_InsertColumn(g_gameList,2,&col);

    g_gameExeBox = CreateWindowW(L"EDIT", NULL, WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL, 670, 440, 248, 26, hwnd, NULL, hi, NULL);
    SendMessageW(g_gameExeBox, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    SendMessageW(g_gameExeBox, EM_SETCUEBANNER, TRUE, (LPARAM)U8(L().exeHint).c_str());
    MakeButton(hwnd, 670, 472, 120, 28, U8(L().gameAdd).c_str(), ID_ADD_GAME);
    MakeButton(hwnd, 798, 472, 120, 28, U8(L().delBind).c_str(), ID_DEL_GAME);
    MakeButton(hwnd, 670, 506, 248, 28, U8(L().gameRefresh).c_str(), ID_REFRESH_GAME);
    g_chkAllGames = MakeCheck(hwnd, 670, 542, 248, 22, U8(L().allGames).c_str(), ID_CHK_ALLGAMES);
    // v1.2.0: 게임 개수 표시
    wchar_t gameCountBuf[128];
    swprintf(gameCountBuf, 128, (g_lang==0 ? L"총 %d개 게임 등록됨" : L"%d games registered"), (int)g_games.size());
    MakeLabel(hwnd, 670, 570, 248, 20, gameCountBuf, g_fontSmall);
    MakeLabel(hwnd, 32, 616, 620, 20, U8(L().gameNote).c_str(), g_fontSmall);

    // ── 하단 ──
    g_memLabel = MakeLabel(hwnd, 24, 644, 900, 20, U8("").c_str(), g_fontSmall);
    g_chkStartup = MakeCheck(hwnd, 24, 668, 170, 22, U8(L().startup).c_str(), ID_CHK_STARTUP);
    g_chkTray = MakeCheck(hwnd, 200, 668, 190, 22, U8(L().trayMin).c_str(), ID_CHK_TRAY);
    g_langLabel = MakeLabel(hwnd, 400, 670, 60, 20, U8(L().langLabel).c_str(), g_fontNormal);
    g_langCombo = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL, 460, 666, 160, 200, hwnd, (HMENU)(INT_PTR)ID_LANG_COMBO, hi, NULL);
    SendMessageW(g_langCombo, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    for(int i=0;i<LANG_COUNT;i++) SendMessageW(g_langCombo, CB_ADDSTRING, 0, (LPARAM)U8(LANG_NAMES[i]).c_str());
    SendMessageW(g_langCombo, CB_SETCURSEL, (WPARAM)g_lang, 0);
    MakeButton(hwnd, 680, 660, 120, 32, U8(L().trayHide).c_str(), ID_TRAY_BTN);
    MakeButton(hwnd, 810, 660, 120, 32, U8(L().exitBtn).c_str(), ID_EXIT_BTN);
    g_logLabel = MakeLabel(hwnd, 24, 698, 908, 20, U8(L().logReady).c_str(), g_fontSmall);

    // 그룹박스 폰트
    EnumChildWindows(hwnd, [](HWND h, LPARAM lp)->BOOL{
        wchar_t cls[32]; GetClassNameW(h, cls, 32);
        if(wcscmp(cls,L"Button")==0){
            wchar_t txt[128]; GetWindowTextW(h, txt, 128);
            if(txt[0]>='1'&&txt[0]<='3'&&txt[1]=='.') SendMessageW(h, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
        }
        (void)lp; return TRUE;
    }, 0);
}
static void UpdateModeButtons(){
    if(g_modeHold) SetWindowTextW(g_modeHold, g_mode==0?U8(L().holdOn).c_str():U8(L().holdOff).c_str());
    if(g_modeToggle) SetWindowTextW(g_modeToggle, g_mode==1?U8(L().toggleOn).c_str():U8(L().toggleOff).c_str());
}

// ---------- 언어 변경: 저장 후 재시작 (폰트까지 새 언어로) ----------
static void ExitApp(){
    g_forceExit = true;
    if(g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}
static void RestartWithNewLanguage(){
    SaveSettings();
    if(g_ducked) RestoreAllVolumes();
    ShellExecuteW(NULL, L"open", g_exePath, NULL, NULL, SW_SHOWNORMAL);
    ExitApp();
}

// ---------- WndProc ----------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    switch(msg){
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateUI(hwnd);
        RefreshAllUI();
        UpdateModeButtons();
        SetTimer(hwnd, TIMER_REFRESH, 2000, NULL);
        AddTray();
        if(g_enabled) InstallHooks();
        SetStatus(g_enabled ? U8(L().statusEnabled) : U8(L().statusDisabled));
        return 0;
    case WM_APP_DUCK_ON:
        StartDuck(true);
        { wchar_t sb[128]; swprintf(sb,128,U8(L().statusDuckFmt).c_str(), g_duckLevel); SetStatus(sb); }
        RefreshAllUI();
        return 0;
    case WM_APP_DUCK_OFF:
        StartDuck(false);
        SetStatus(U8(L().statusRestored));
        RefreshAllUI();
        return 0;
    case WM_APP+20: // 바인딩 목록 갱신
        RefreshBindingList();
        return 0;
    case WM_TIMER:
        if(wParam==TIMER_REFRESH){
            RefreshGameList();
            wchar_t buf[160];
            swprintf(buf,160,U8(L().memFmt).c_str(), CurrentMemoryMB());
            if(g_memLabel) SetWindowTextW(g_memLabel, buf);
        } else if(wParam==TIMER_FADE){
            DWORD now = GetTickCount();
            DWORD total = (DWORD)(g_fadeMs<=0?1:g_fadeMs);
            float p = (float)(now - g_fadeStartMs)/(float)total;
            if(p>=1.0f){ p=1.0f; g_fadeActive=false; KillTimer(hwnd, TIMER_FADE); }
            float prog = g_fadeTargetDuck ? p : (1.0f-p);
            g_fadeProgress = prog;
            ApplyVolumeForDuckState(prog);
            if(p>=1.0f && !g_fadeTargetDuck) RestoreAllVolumes();
        } else if(wParam==TIMER_CAPTURE){
            if(g_capturing){
                g_capturing=false;
                SetStatus(U8(L().msgTimeout));
                if(g_captureLabel) SetWindowTextW(g_captureLabel, U8(L().msgTimeout).c_str());
            }
            KillTimer(hwnd, TIMER_CAPTURE);
        }
        return 0;
    case WM_HSCROLL:
        if((HWND)lParam==g_duckSlider){
            g_duckLevel=(int)SendMessageW(g_duckSlider, TBM_GETPOS, 0, 0);
            RefreshAllUI(); SaveSettings();
        } else if((HWND)lParam==g_fadeSlider){
            g_fadeMs=(int)SendMessageW(g_fadeSlider, TBM_GETPOS, 0, 0);
            RefreshAllUI(); SaveSettings();
        }
        return 0;
    case WM_NOTIFY:
        if(((LPNMHDR)lParam)->hwndFrom==g_gameList && ((LPNMHDR)lParam)->code==LVN_ITEMCHANGED){
            LPNMLISTVIEW pnm=(LPNMLISTVIEW)lParam;
            if(pnm->uChanged & LVIF_STATE){
                // v1.2.0: lParam에서 원래 게임 인덱스를 꺼낸다
                LVITEMW lvi={0}; lvi.mask=LVIF_PARAM; lvi.iItem=pnm->iItem;
                ListView_GetItem(g_gameList, &lvi);
                int gi = (int)lvi.lParam;
                BOOL checked = ListView_GetCheckState(g_gameList, pnm->iItem);
                if(gi>=0 && gi<(int)g_games.size()){
                    bool nw = checked?true:false;
                    if(g_games[gi].enabled!=nw){ g_games[gi].enabled=nw; SaveSettings(); }
                }
            }
        }
        return 0;
    case WM_COMMAND:{
        int id=LOWORD(wParam);
        // v1.2.0: 게임 검색 필터
        if(id==ID_GAME_SEARCH && HIWORD(wParam)==EN_CHANGE){
            wchar_t buf[256]=L"";
            GetWindowTextW(g_gameSearchBox, buf, 256);
            g_gameFilter = buf;
            // 필터 변경 시 목록 전체를 다시 그린다 (스크롤 보존 로직은 RefreshGameList 내부에서 처리)
            ListView_DeleteAllItems(g_gameList);
            RefreshGameList();
            return 0;
        }
        if(id==ID_LANG_COMBO && HIWORD(wParam)==CBN_SELCHANGE){
            int sel=(int)SendMessageW(g_langCombo, CB_GETCURSEL, 0, 0);
            if(sel>=0 && sel<LANG_COUNT && sel!=g_lang){
                g_lang=sel;
                RestartWithNewLanguage();
            }
            return 0;
        }
        if(id==ID_ADD_BIND){
            g_capturing=true; g_captureStart=GetTickCount();
            SetTimer(hwnd, TIMER_CAPTURE, 15000, NULL);
            SetStatus(U8(L().captureHint));
            if(g_captureLabel) SetWindowTextW(g_captureLabel, U8(L().captureHint).c_str());
            SetFocus(hwnd);
        } else if(id==ID_DEL_BIND){
            int sel=(int)SendMessageW(g_bindList, LB_GETCURSEL, 0, 0);
            if(sel>=0 && sel<(int)g_bindings.size()){ g_bindings.erase(g_bindings.begin()+sel); SaveSettings(); RefreshBindingList(); }
        } else if(id==ID_CLEAR_BIND){
            g_bindings.clear(); SaveSettings(); RefreshBindingList();
        } else if(id==ID_MODE_HOLD){ g_mode=0; SaveSettings(); UpdateModeButtons(); SetStatus(U8(L().msgModeHold)); }
        else if(id==ID_MODE_TOGGLE){ g_mode=1; SaveSettings(); UpdateModeButtons(); SetStatus(U8(L().msgModeToggle)); }
        else if(id==ID_TEST_DUCK){
            if(!g_ducked){ StartDuck(true); SetStatus(U8(L().msgTest)); }
            else { StartDuck(false); SetStatus(U8(L().statusRestored)); }
            RefreshAllUI();
        } else if(id==ID_RESTORE_VOL){
            g_toggleState=false; g_holdCount=0; StartDuck(false); SetStatus(U8(L().statusRestored)); RefreshAllUI();
        } else if(id==ID_ADD_GAME){
            wchar_t buf[MAX_PATH]=L""; GetWindowTextW(g_gameExeBox, buf, MAX_PATH);
            std::wstring exe=buf;
            if(exe==U8(L().exeHint) || exe.rfind(U8("예:"),0)==0 || exe.rfind(U8("e.g."),0)==0 || exe.rfind(U8("ex.:"),0)==0 || exe.rfind(U8("p. ej."),0)==0 || exe.rfind(U8("p. ex."),0)==0 || exe.rfind(U8("z. B."),0)==0 || exe.rfind(U8("напр."),0)==0 || exe.rfind(U8("örn."),0)==0 || exe.rfind(U8("vd:"),0)==0 || exe.rfind(U8("cth:"),0)==0 || exe.rfind(U8("例如:"),0)==0 || exe.rfind(U8("例如："),0)==0) exe=L"";
            while(!exe.empty()&&(exe.back()==L' '||exe.back()==L'\r'||exe.back()==L'\n')) exe.pop_back();
            while(!exe.empty()&&exe.front()==L' ') exe.erase(exe.begin());
            if(!exe.empty()){
                if(exe.find(L".exe")==std::wstring::npos && exe.find(L".EXE")==std::wstring::npos) exe+=L".exe";
                Game g; g.exe=exe;
                std::wstring name=exe; size_t dot=name.rfind(L'.'); if(dot!=std::wstring::npos) name=name.substr(0,dot);
                g.name=name; g.enabled=true;
                bool dup=false; for(auto& gg: g_games) if(ToLower(gg.exe)==ToLower(exe)) dup=true;
                if(!dup){ g_games.push_back(g); SaveSettings(); RefreshGameList(); SetStatus(U8(L().msgGameAdded)+exe); }
                else SetStatus(U8(L().msgGameDup)+exe);
            }
        } else if(id==ID_DEL_GAME){
            int sel=ListView_GetNextItem(g_gameList, -1, LVNI_SELECTED);
            if(sel>=0){
                // v1.2.0: 필터링 중이면 lParam에서 원래 인덱스를 꺼낸다
                LVITEMW lvi={0}; lvi.mask=LVIF_PARAM; lvi.iItem=sel;
                ListView_GetItem(g_gameList, &lvi);
                int gi = (int)lvi.lParam;
                if(gi>=0 && gi<(int)g_games.size()){ g_games.erase(g_games.begin()+gi); SaveSettings(); RefreshGameList(); }
            }
        } else if(id==ID_REFRESH_GAME){
            RefreshGameList(); SetStatus(U8(L().msgRefreshed));
        } else if(id==ID_TRAY_BTN){
            ShowWindow(hwnd, SW_HIDE);
            SetStatus(g_lang==0?U8("트레이에서 실행 중 — 아이콘을 더블클릭하면 돌아옵니다"):U8("Running in tray — double-click the icon to return"));
        } else if(id==ID_EXIT_BTN){
            ExitApp();
        } else if(id==ID_CHK_ENABLE){
            g_enabled = SendMessageW(g_chkEnable, BM_GETCHECK, 0, 0)==BST_CHECKED;
            if(g_enabled) InstallHooks(); else { UninstallHooks(); if(g_ducked){ g_holdCount=0; g_toggleState=false; StartDuck(false); } }
            SaveSettings(); SetStatus(g_enabled?U8(L().statusEnabled):U8(L().statusDisabled));
        } else if(id==ID_CHK_STARTUP){
            g_startWithWindows = SendMessageW(g_chkStartup, BM_GETCHECK, 0, 0)==BST_CHECKED;
            ApplyStartup(); SaveSettings();
        } else if(id==ID_CHK_TRAY){
            g_minToTray = SendMessageW(g_chkTray, BM_GETCHECK, 0, 0)==BST_CHECKED; SaveSettings();
        } else if(id==ID_CHK_ALLGAMES){
            g_allGames = SendMessageW(g_chkAllGames, BM_GETCHECK, 0, 0)==BST_CHECKED; SaveSettings();
            SetStatus(g_allGames?U8(L().allGames):U8(L().grp3));
        }
        return 0;
    }
    case WM_TRAYICON:
        if(lParam==WM_LBUTTONDBLCLK){ ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
        else if(lParam==WM_RBUTTONUP){
            POINT pt; GetCursorPos(&pt);
            HMENU menu=CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, (g_lang==0?U8("열기"):U8("Open")).c_str());
            AppendMenuW(menu, MF_STRING, ID_TRAY_TOGGLE, g_ducked?U8(L().restoreVol).c_str():U8(L().testDuck).c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, U8(L().exitBtn).c_str());
            SetForegroundWindow(hwnd);
            int cmd=TrackPopupMenu(menu, TPM_RETURNCMD|TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(menu);
            if(cmd==ID_TRAY_OPEN){ ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
            else if(cmd==ID_TRAY_TOGGLE){ if(g_ducked) PostMessageW(hwnd, WM_APP_DUCK_OFF,0,0); else PostMessageW(hwnd, WM_APP_DUCK_ON,0,0); }
            else if(cmd==ID_TRAY_EXIT){ ExitApp(); }
        }
        return 0;
    case WM_CLOSE:
        if(g_minToTray && !g_forceExit){
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if(g_ducked) RestoreAllVolumes();
        UninstallHooks();
        RemoveTray();
        KillTimer(hwnd, TIMER_REFRESH); KillTimer(hwnd, TIMER_FADE); KillTimer(hwnd, TIMER_CAPTURE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow){
    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);
    LoadSettings();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX ice={sizeof(ice), ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES};
    InitCommonControlsEx(&ice);
    WNDCLASSW wc={0};
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName=L"GameDuckWindow";
    wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor=LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon=(HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, 0);
    if(!wc.hIcon) wc.hIcon=LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);
    // v1.2.0: 창 크기 확대 (960x760)
    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    int W=960, H=760;
    HWND hwnd=CreateWindowW(L"GameDuckWindow", (g_lang==0?U8("GameDuck v1.2.0 — 게임 볼륨 즉시 낮추기"):U8("GameDuck v1.2.0 — Instant Game Volume Ducker")).c_str(),
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        (sw-W)/2, (sh-H)/2, W, H, NULL, NULL, hInst, NULL);
    if(!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    MSG msg;
    while(GetMessageW(&msg, NULL, 0, 0)){
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
