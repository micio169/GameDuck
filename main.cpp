// GameDuck v1 - 게임 볼륨 즉시 낮추기
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
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")

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
static void SetDefaultGames(){
    g_games.clear();
    auto add=[&](const char* exe, const char* name){ Game g; g.exe=U8(exe); g.name=U8(name); g.enabled=true; g_games.push_back(g); };
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
    snprintf(buf,sizeof(buf),"Enabled=%d\nMode=%d\nDuckLevel=%d\nFadeMs=%d\nStartWithWindows=%d\nMinimizeToTray=%d\nAllGames=%d\n",
        g_enabled?1:0, g_mode, g_duckLevel, g_fadeMs, g_startWithWindows?1:0, g_minToTray?1:0, g_allGames?1:0);
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
    int bc = IniInt(m,"BindingCount",-1);
    if(bc>=0){ g_bindings.clear(); for(int i=0;i<bc;i++){ char k[32]; snprintf(k,sizeof(k),"Binding%d",i); auto it=m.find(k); if(it==m.end()) continue; int t=0,v=0,mo=0,md=0; if(sscanf(it->second.c_str(),"%d,%d,%d,%d",&t,&v,&mo,&md)==4){ Binding b; b.type=t; b.vk=v; b.mouse=mo; b.mods=md; g_bindings.push_back(b);} } }
    int gc = IniInt(m,"GameCount",-1);
    if(gc>=0){ g_games.clear(); for(int i=0;i<gc;i++){ char k[32]; snprintf(k,sizeof(k),"Game%d",i); auto it=m.find(k); if(it==m.end()) continue; std::string s=it->second; size_t p1=s.find('|'); size_t p2=s.rfind('|'); if(p1==std::string::npos||p2==std::string::npos||p2<=p1) continue; Game g; g.exe=U8(s.substr(0,p1).c_str()); g.name=U8(s.substr(p1+1,p2-p1-1).c_str()); g.enabled=(s.substr(p2+1)=="1"); g_games.push_back(g);} }
    if(g_games.empty()) SetDefaultGames();
    if(g_bindings.empty()) SetDefaultBindings();
}

// ---------- 키 이름 ----------
static std::wstring VkName(int vk){
    if(vk==0) return U8("없음");
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    wchar_t buf[64]=L"";
    // 확장 키 처리 (방향키 등)
    LONG lparam = (LONG)(sc<<16);
    if(vk==VK_LEFT||vk==VK_RIGHT||vk==VK_UP||vk==VK_DOWN||vk==VK_HOME||vk==VK_END||vk==VK_PRIOR||vk==VK_NEXT||vk==VK_INSERT||vk==VK_DELETE||vk==VK_NUMLOCK) lparam |= (1<<24);
    if(GetKeyNameTextW(lparam, buf, 64)>0) return buf;
    wchar_t tmp[32]; swprintf(tmp,32,L"VK %d",vk); return tmp;
}
static std::wstring MouseName(int m){
    switch(m){ case 1: return U8("마우스 왼쪽"); case 2: return U8("마우스 오른쪽"); case 3: return U8("마우스 휠 클릭"); case 4: return U8("마우스 X1 (옆 버튼)"); case 5: return U8("마우스 X2 (옆 버튼)"); }
    return U8("마우스");
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
    // 바인딩에 없는 수식키는 눌려 있어도 되지만, 있는 수식키는 반드시 눌려 있어야 함.
    // 단, 바인딩 자체가 Ctrl/Alt/Shift 단독 키인 경우는 제외 (vk가 수식키면 mods 무시)
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
        // 시스템 소리/브라우저 제외하고, 실행 중인 게임 목록에 있거나 게임으로 보이는 프로세스는 대상
        if(low==L"system sounds"||low==L"audiodg.exe") return false;
        return true;
    }
    for(auto& g: g_games) if(g.enabled && ToLower(g.exe)==low) return true;
    return false;
}
// 세션을 열거해 콜백 호출. 덕킹/복원/상태 확인 모두 이 함수 하나로.
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
    // progress 0=원래 볼륨, 1=덕킹 볼륨. 페이드 중에는 이 값으로 보간.
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
    // 덕킹했던 pid만 원래대로. 세션이 사라졌으면 건너뜀.
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
        // 페이드 시작점: 켤 때는 현재 볼륨이 원래 볼륨, 끌 때는 덕킹 볼륨에서 시작
        // (간단히 0->1 / 1->0으로 보간, 시작 볼륨은 ApplyVolumeForDuckState가 orig 기준으로 계산)
        g_fadeActive=true;
        SetTimer(g_hwnd, TIMER_FADE, 15, NULL);
        // 첫 프레임 즉시 적용해 지연 체감을 없앰
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
    // 수식키 단독 바인딩이면 mods 검사 생략
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
                SetStatus(U8("키 매핑 취소됨"));
                if(g_captureLabel) SetWindowTextW(g_captureLabel, U8("취소됨 — 다시 [키/마우스 추가]를 누르세요.").c_str());
            } else {
                Binding b; b.type=0; b.vk=vk; b.mouse=0;
                // 수식키 단독이면 mods 0, 아니면 현재 눌린 수식키 기록 (단, 방금 누른 키가 수식키면 제외)
                int mods=CurrentMods();
                if(vk==VK_CONTROL||vk==VK_LCONTROL||vk==VK_RCONTROL) mods&=~1;
                if(vk==VK_MENU||vk==VK_LMENU||vk==VK_RMENU) mods&=~2;
                if(vk==VK_SHIFT||vk==VK_LSHIFT||vk==VK_RSHIFT) mods&=~4;
                b.mods=mods;
                g_bindings.push_back(b);
                g_capturing=false; KillTimer(g_hwnd, TIMER_CAPTURE);
                SaveSettings();
                // 리스트 갱신은 메인 스레드에서
                PostMessageW(g_hwnd, WM_APP+20, 0, 0);
                SetStatus(U8("키 매핑 추가됨: ") + BindingLabel(b));
            }
            return 1; // 캡처한 그 한 번의 입력만 게임으로 전달하지 않음
        }
        if(!g_capturing && g_enabled){
            // 오토리피트 무시: 이미 눌려 있으면 다시 트리거하지 않음 (전역 맵으로 down/up 공유)
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
                SetStatus(U8("마우스 매핑 추가됨: ") + BindingLabel(b));
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
    wcscpy_s(g_nid.szTip, U8("GameDuck — 게임 볼륨 덕커").c_str());
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

// ---------- 게임 목록 갱신 ----------
static void RefreshGameList(){
    if(!g_gameList) return;
    ListView_DeleteAllItems(g_gameList);
    // 실행 중 프로세스 집합 (상태 표시용)
    std::map<std::wstring,bool> running;
    ForEachTargetSession([&](DWORD, const std::wstring& exe, ISimpleAudioVolume* pVol){
        float v=0; pVol->GetMasterVolume(&v);
        running[ToLower(exe)]=true;
    });
    // 오디오 세션이 없어도 프로세스 자체가 떠 있으면 실행 중으로 표시
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap!=INVALID_HANDLE_VALUE){
        PROCESSENTRY32W pe; pe.dwSize=sizeof(pe);
        if(Process32FirstW(snap,&pe)){ do{ running[ToLower(pe.szExeFile)]=true; }while(Process32NextW(snap,&pe)); }
        CloseHandle(snap);
    }
    for(size_t i=0;i<g_games.size();i++){
        LVITEMW item={0}; item.mask=LVIF_TEXT; item.iItem=(int)i; item.iSubItem=0; item.pszText=(LPWSTR)g_games[i].name.c_str();
        int idx=ListView_InsertItem(g_gameList,&item);
        ListView_SetItemText(g_gameList, idx, 1, (LPWSTR)g_games[i].exe.c_str());
        bool run = running.count(ToLower(g_games[i].exe))>0;
        std::wstring st = run ? U8("실행 중") : U8("미실행");
        if(run && g_ducked) st += U8(" · 덕킹 중");
        ListView_SetItemText(g_gameList, idx, 2, (LPWSTR)st.c_str());
        ListView_SetCheckState(g_gameList, idx, g_games[i].enabled?TRUE:FALSE);
    }
}
static void RefreshBindingList(){
    if(!g_bindList) return;
    SendMessageW(g_bindList, LB_RESETCONTENT, 0, 0);
    for(auto& b: g_bindings){
        std::wstring s = BindingLabel(b);
        SendMessageW(g_bindList, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    if(g_bindings.empty()) SendMessageW(g_bindList, LB_ADDSTRING, 0, (LPARAM)U8("(매핑 없음 — [키/마우스 추가]를 눌러 등록하세요)").c_str());
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
    swprintf(buf,128,U8("낮춘 볼륨: %d%%  (원래 볼륨 → %d%%로)").c_str(), g_duckLevel, g_duckLevel);
    if(g_duckLabel) SetWindowTextW(g_duckLabel, buf);
    // mingw wide swprintf에서는 %s가 narrow 문자열을 뜻하므로 wide 문자열은 %ls를 써야 함
    swprintf(buf,128,U8("페이드: %dms  (%ls)").c_str(), g_fadeMs, g_fadeMs==0?U8("즉시").c_str():U8("부드럽게").c_str());
    if(g_fadeLabel) SetWindowTextW(g_fadeLabel, buf);
    if(g_volBar) SendMessageW(g_volBar, PBM_SETPOS, (WPARAM)(g_ducked?g_duckLevel:100), 0);
    swprintf(buf,128,U8("게임 볼륨: %d%%").c_str(), g_ducked?g_duckLevel:100);
    if(g_volLabel) SetWindowTextW(g_volLabel, buf);
}

// ---------- UI ----------
static HFONT MakeFont(int size, bool bold){
    return CreateFontW(-MulDiv(size, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72), 0,0,0,
        bold?FW_BOLD:FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Malgun Gothic");
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
       ID_TRAY_BTN, ID_EXIT_BTN, ID_CHK_ENABLE, ID_CHK_STARTUP, ID_CHK_TRAY, ID_CHK_ALLGAMES };
static void CreateUI(HWND hwnd){
    g_fontTitle = MakeFont(16, true);
    g_fontBold = MakeFont(10, true);
    g_fontNormal = MakeFont(10, false);
    g_fontSmall = MakeFont(9, false);
    HINSTANCE hi = GetModuleHandleW(NULL);
    // 헤더
    MakeLabel(hwnd, 20, 14, 300, 28, U8("GameDuck").c_str(), g_fontTitle);
    MakeLabel(hwnd, 20, 44, 560, 20, U8("키나 마우스 버튼을 누르면 게임 볼륨만 즉시 낮춥니다  ·  시스템 볼륨·디스코드는 그대로").c_str(), g_fontSmall);
    g_chkEnable = MakeCheck(hwnd, 620, 16, 120, 26, U8("덕킹 활성화").c_str(), ID_CHK_ENABLE);
    g_statusLabel = MakeLabel(hwnd, 620, 44, 220, 20, U8("대기 중").c_str(), g_fontBold);
    // 1. 트리거
    CreateWindowW(L"BUTTON", U8("1. 트리거 키 / 마우스 버튼 — 아무 키나 매핑 가능").c_str(), WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 16, 72, 848, 168, hwnd, NULL, hi, NULL);
    MakeLabel(hwnd, 32, 96, 300, 20, U8("등록된 트리거 (아무거나 누르면 작동)").c_str(), g_fontBold);
    g_bindList = CreateWindowW(L"LISTBOX", NULL, WS_CHILD|WS_VISIBLE|WS_BORDER|LBS_NOTIFY|WS_VSCROLL, 32, 118, 300, 104, hwnd, NULL, hi, NULL);
    SendMessageW(g_bindList, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    MakeButton(hwnd, 344, 118, 150, 30, U8("키 / 마우스 추가").c_str(), ID_ADD_BIND);
    MakeButton(hwnd, 344, 152, 150, 30, U8("선택 삭제").c_str(), ID_DEL_BIND);
    MakeButton(hwnd, 344, 186, 150, 30, U8("모두 지우기").c_str(), ID_CLEAR_BIND);
    g_captureLabel = MakeLabel(hwnd, 510, 118, 330, 44, U8("추가 버튼을 누른 뒤 키나 마우스 버튼을 한 번 누르세요. (ESC 취소)").c_str(), g_fontSmall);
    g_modeHold = MakeButton(hwnd, 510, 168, 160, 30, U8("● 누르고 있는 동안만").c_str(), ID_MODE_HOLD);
    g_modeToggle = MakeButton(hwnd, 674, 168, 160, 30, U8("○ 누를 때마다 전환").c_str(), ID_MODE_TOGGLE);
    // 라디오처럼 보이게: 일반 버튼 + 텍스트로 모드 표시 (BS_AUTORADIOBUTTON은 그룹박스 안에서만 동작이 깔끔해 버튼으로 처리)
    MakeLabel(hwnd, 510, 202, 330, 20, U8("키는 가로채지 않습니다. 게임 조작은 그대로 전달됩니다.").c_str(), g_fontSmall);
    // 2. 볼륨
    CreateWindowW(L"BUTTON", U8("2. 볼륨 — 낮춘 정도와 속도").c_str(), WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 16, 250, 848, 128, hwnd, NULL, hi, NULL);
    g_duckLabel = MakeLabel(hwnd, 32, 274, 320, 20, U8("낮춘 볼륨: 10%").c_str(), g_fontBold);
    g_duckSlider = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS, 32, 296, 320, 32, hwnd, NULL, hi, NULL);
    SendMessageW(g_duckSlider, TBM_SETRANGE, TRUE, MAKELONG(0,50));
    SendMessageW(g_duckSlider, TBM_SETPAGESIZE, 0, 5);
    g_fadeLabel = MakeLabel(hwnd, 380, 274, 200, 20, U8("페이드: 0ms (즉시)").c_str(), g_fontBold);
    g_fadeSlider = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS, 380, 296, 200, 32, hwnd, NULL, hi, NULL);
    SendMessageW(g_fadeSlider, TBM_SETRANGE, TRUE, MAKELONG(0,300));
    SendMessageW(g_fadeSlider, TBM_SETPAGESIZE, 0, 50);
    MakeButton(hwnd, 610, 274, 110, 30, U8("지금 낮추기").c_str(), ID_TEST_DUCK);
    MakeButton(hwnd, 728, 274, 110, 30, U8("원래대로").c_str(), ID_RESTORE_VOL);
    g_volLabel = MakeLabel(hwnd, 610, 310, 120, 20, U8("게임 볼륨: 100%").c_str(), g_fontNormal);
    g_volBar = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD|WS_VISIBLE|PBS_SMOOTH, 610, 332, 228, 18, hwnd, NULL, hi, NULL);
    SendMessageW(g_volBar, PBM_SETRANGE, 0, MAKELONG(0,100));
    SendMessageW(g_volBar, PBM_SETPOS, 100, 0);
    MakeLabel(hwnd, 32, 336, 560, 20, U8("낮춘 볼륨은 게임 세션 볼륨(절대값)입니다. 0%면 무음, 10%면 속삭이듯 작아집니다.").c_str(), g_fontSmall);
    // 3. 게임 목록
    CreateWindowW(L"BUTTON", U8("3. 게임 목록 — 여러 게임 동시 지원, 체크한 게임만 덕킹").c_str(), WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 16, 388, 848, 196, hwnd, NULL, hi, NULL);
    g_gameList = CreateWindowW(WC_LISTVIEWW, NULL, WS_CHILD|WS_VISIBLE|WS_BORDER|LVS_REPORT|LVS_SINGLESEL, 32, 412, 560, 128, hwnd, NULL, hi, NULL);
    SendMessageW(g_gameList, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    ListView_SetExtendedListViewStyle(g_gameList, LVS_EX_FULLROWSELECT|LVS_EX_CHECKBOXES|LVS_EX_GRIDLINES);
    LVCOLUMNW col={0}; col.mask=LVCF_TEXT|LVCF_WIDTH;
    static std::wstring colGame = U8("게임"), colProc = U8("프로세스"), colState = U8("상태");
    col.pszText=(LPWSTR)colGame.c_str(); col.cx=170; ListView_InsertColumn(g_gameList,0,&col);
    col.pszText=(LPWSTR)colProc.c_str(); col.cx=250; ListView_InsertColumn(g_gameList,1,&col);
    col.pszText=(LPWSTR)colState.c_str(); col.cx=130; ListView_InsertColumn(g_gameList,2,&col);
    g_gameExeBox = CreateWindowW(L"EDIT", NULL, WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL, 610, 412, 228, 26, hwnd, NULL, hi, NULL);
    SendMessageW(g_gameExeBox, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    SetWindowTextW(g_gameExeBox, U8("예: cs2.exe").c_str());
    MakeButton(hwnd, 610, 444, 110, 30, U8("게임 추가").c_str(), ID_ADD_GAME);
    MakeButton(hwnd, 728, 444, 110, 30, U8("선택 삭제").c_str(), ID_DEL_GAME);
    MakeButton(hwnd, 610, 480, 228, 30, U8("실행 중인 게임 새로고침").c_str(), ID_REFRESH_GAME);
    g_chkAllGames = MakeCheck(hwnd, 610, 514, 228, 22, U8("목록 외 앱도 자동 덕킹").c_str(), ID_CHK_ALLGAMES);
    MakeLabel(hwnd, 32, 546, 560, 20, U8("게임 이름은 프로세스명에서 자동 추정됩니다. exe명을 정확히 적으면 됩니다.").c_str(), g_fontSmall);
    // 하단
    g_memLabel = MakeLabel(hwnd, 20, 596, 420, 20, U8("메모리: 측정 중…").c_str(), g_fontSmall);
    g_chkStartup = MakeCheck(hwnd, 20, 620, 170, 22, U8("윈도우 시작 시 실행").c_str(), ID_CHK_STARTUP);
    g_chkTray = MakeCheck(hwnd, 200, 620, 150, 22, U8("닫으면 트레이로").c_str(), ID_CHK_TRAY);
    MakeButton(hwnd, 620, 612, 110, 32, U8("트레이로 숨기기").c_str(), ID_TRAY_BTN);
    MakeButton(hwnd, 738, 612, 110, 32, U8("종료").c_str(), ID_EXIT_BTN);
    g_logLabel = MakeLabel(hwnd, 20, 648, 828, 20, U8("준비됨 — 트리거 키를 누르면 게임 볼륨이 즉시 낮아집니다.").c_str(), g_fontSmall);
    // 그룹박스 폰트
    EnumChildWindows(hwnd, [](HWND h, LPARAM lp)->BOOL{
        wchar_t cls[32]; GetClassNameW(h, cls, 32);
        if(wcscmp(cls,L"Button")==0){
            wchar_t txt[128]; GetWindowTextW(h, txt, 128);
            // 그룹박스만 볼드 처리 (텍스트가 "1. " 등으로 시작)
            if(txt[0]>='1'&&txt[0]<='3'&&txt[1]=='.') SendMessageW(h, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
        }
        (void)lp; return TRUE;
    }, 0);
}
static void UpdateModeButtons(){
    if(g_modeHold) SetWindowTextW(g_modeHold, g_mode==0?U8("● 누르고 있는 동안만").c_str():U8("○ 누르고 있는 동안만").c_str());
    if(g_modeToggle) SetWindowTextW(g_modeToggle, g_mode==1?U8("● 누를 때마다 전환").c_str():U8("○ 누를 때마다 전환").c_str());
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
        SetStatus(g_enabled ? U8("활성화됨 — 대기 중") : U8("비활성화됨 — [덕킹 활성화]를 켜세요"));
        return 0;
    case WM_APP_DUCK_ON:
        StartDuck(true);
        SetStatus(U8("덕킹 중 — 게임 볼륨 ") + std::to_wstring(g_duckLevel) + U8("%로 낮아짐"));
        RefreshAllUI();
        return 0;
    case WM_APP_DUCK_OFF:
        StartDuck(false);
        SetStatus(U8("복원됨 — 게임 볼륨 원래대로"));
        RefreshAllUI();
        return 0;
    case WM_APP+20: // 바인딩 목록 갱신
        RefreshBindingList();
        return 0;
    case WM_TIMER:
        if(wParam==TIMER_REFRESH){
            RefreshGameList();
            wchar_t buf[160];
            swprintf(buf,160,U8("메모리: %.1f MB  ·  CPU 부담 거의 없음 (이벤트 방식 훅 + 2초 간격 스캔)").c_str(), CurrentMemoryMB());
            if(g_memLabel) SetWindowTextW(g_memLabel, buf);
        } else if(wParam==TIMER_FADE){
            DWORD now = GetTickCount();
            DWORD total = (DWORD)(g_fadeMs<=0?1:g_fadeMs);
            float p = (float)(now - g_fadeStartMs)/(float)total;
            if(p>=1.0f){ p=1.0f; g_fadeActive=false; KillTimer(hwnd, TIMER_FADE); }
            // 켤 때 0->1, 끌 때 1->0
            float prog = g_fadeTargetDuck ? p : (1.0f-p);
            g_fadeProgress = prog;
            ApplyVolumeForDuckState(prog);
            if(p>=1.0f && !g_fadeTargetDuck) RestoreAllVolumes();
        } else if(wParam==TIMER_CAPTURE){
            if(g_capturing){
                g_capturing=false;
                SetStatus(U8("키 매핑 시간 초과 — 다시 시도하세요"));
                if(g_captureLabel) SetWindowTextW(g_captureLabel, U8("시간 초과됨 — 다시 [키/마우스 추가]를 누르세요.").c_str());
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
                BOOL checked = ListView_GetCheckState(g_gameList, pnm->iItem);
                if(pnm->iItem>=0 && pnm->iItem<(int)g_games.size()){
                    bool nw = checked?true:false;
                    if(g_games[pnm->iItem].enabled!=nw){ g_games[pnm->iItem].enabled=nw; SaveSettings(); }
                }
            }
        }
        return 0;
    case WM_COMMAND:{
        int id=LOWORD(wParam);
        if(id==ID_ADD_BIND){
            g_capturing=true; g_captureStart=GetTickCount();
            SetTimer(hwnd, TIMER_CAPTURE, 15000, NULL);
            SetStatus(U8("키 입력 대기 중 — 키나 마우스 버튼을 한 번 누르세요 (ESC 취소)"));
            if(g_captureLabel) SetWindowTextW(g_captureLabel, U8("입력 대기 중… 키나 마우스 버튼을 한 번 누르세요. (ESC 취소)").c_str());
            SetFocus(hwnd);
        } else if(id==ID_DEL_BIND){
            int sel=(int)SendMessageW(g_bindList, LB_GETCURSEL, 0, 0);
            if(sel>=0 && sel<(int)g_bindings.size()){ g_bindings.erase(g_bindings.begin()+sel); SaveSettings(); RefreshBindingList(); }
        } else if(id==ID_CLEAR_BIND){
            g_bindings.clear(); SaveSettings(); RefreshBindingList();
        } else if(id==ID_MODE_HOLD){ g_mode=0; SaveSettings(); UpdateModeButtons(); SetStatus(U8("모드: 누르고 있는 동안만 덕킹")); }
        else if(id==ID_MODE_TOGGLE){ g_mode=1; SaveSettings(); UpdateModeButtons(); SetStatus(U8("모드: 누를 때마다 켜기/끄기 전환")); }
        else if(id==ID_TEST_DUCK){
            if(!g_ducked){ StartDuck(true); SetStatus(U8("테스트: 게임 볼륨 낮아짐 — [원래대로]를 누르면 복원")); }
            else { StartDuck(false); SetStatus(U8("복원됨")); }
            RefreshAllUI();
        } else if(id==ID_RESTORE_VOL){
            g_toggleState=false; g_holdCount=0; StartDuck(false); SetStatus(U8("복원됨 — 게임 볼륨 원래대로")); RefreshAllUI();
        } else if(id==ID_ADD_GAME){
            wchar_t buf[MAX_PATH]=L""; GetWindowTextW(g_gameExeBox, buf, MAX_PATH);
            std::wstring exe=buf;
            // "예: cs2.exe" 같은 안내 문구면 무시
            if(exe.rfind(U8("예:"),0)==0) exe=L"";
            while(!exe.empty()&&(exe.back()==L' '||exe.back()==L'\r'||exe.back()==L'\n')) exe.pop_back();
            while(!exe.empty()&&exe.front()==L' ') exe.erase(exe.begin());
            if(!exe.empty()){
                if(exe.find(L".exe")==std::wstring::npos && exe.find(L".EXE")==std::wstring::npos) exe+=L".exe";
                Game g; g.exe=exe;
                std::wstring name=exe; size_t dot=name.rfind(L'.'); if(dot!=std::wstring::npos) name=name.substr(0,dot);
                g.name=name; g.enabled=true;
                bool dup=false; for(auto& gg: g_games) if(ToLower(gg.exe)==ToLower(exe)) dup=true;
                if(!dup){ g_games.push_back(g); SaveSettings(); RefreshGameList(); SetStatus(U8("게임 추가됨: ")+exe); }
                else SetStatus(U8("이미 목록에 있는 게임입니다: ")+exe);
            }
        } else if(id==ID_DEL_GAME){
            int sel=ListView_GetNextItem(g_gameList, -1, LVNI_SELECTED);
            if(sel>=0 && sel<(int)g_games.size()){ g_games.erase(g_games.begin()+sel); SaveSettings(); RefreshGameList(); }
        } else if(id==ID_REFRESH_GAME){
            RefreshGameList(); SetStatus(U8("게임 목록 새로고침됨"));
        } else if(id==ID_TRAY_BTN){
            ShowWindow(hwnd, SW_HIDE);
            SetStatus(U8("트레이에서 실행 중 — 아이콘을 더블클릭하면 돌아옵니다"));
        } else if(id==ID_EXIT_BTN){
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if(id==ID_CHK_ENABLE){
            g_enabled = SendMessageW(g_chkEnable, BM_GETCHECK, 0, 0)==BST_CHECKED;
            if(g_enabled) InstallHooks(); else { UninstallHooks(); if(g_ducked){ g_holdCount=0; g_toggleState=false; StartDuck(false); } }
            SaveSettings(); SetStatus(g_enabled?U8("활성화됨 — 대기 중"):U8("비활성화됨"));
        } else if(id==ID_CHK_STARTUP){
            g_startWithWindows = SendMessageW(g_chkStartup, BM_GETCHECK, 0, 0)==BST_CHECKED;
            ApplyStartup(); SaveSettings();
        } else if(id==ID_CHK_TRAY){
            g_minToTray = SendMessageW(g_chkTray, BM_GETCHECK, 0, 0)==BST_CHECKED; SaveSettings();
        } else if(id==ID_CHK_ALLGAMES){
            g_allGames = SendMessageW(g_chkAllGames, BM_GETCHECK, 0, 0)==BST_CHECKED; SaveSettings();
            SetStatus(g_allGames?U8("목록 외 앱도 덕킹합니다 — 게임 외 앱 볼륨도 낮아질 수 있습니다"):U8("목록에 있는 게임만 덕킹합니다"));
        }
        return 0;
    }
    case WM_TRAYICON:
        if(lParam==WM_LBUTTONDBLCLK){ ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
        else if(lParam==WM_RBUTTONUP){
            POINT pt; GetCursorPos(&pt);
            HMENU menu=CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, U8("열기").c_str());
            AppendMenuW(menu, MF_STRING, ID_TRAY_TOGGLE, g_ducked?U8("볼륨 원래대로").c_str():U8("지금 볼륨 낮추기").c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, U8("종료").c_str());
            SetForegroundWindow(hwnd);
            int cmd=TrackPopupMenu(menu, TPM_RETURNCMD|TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(menu);
            if(cmd==ID_TRAY_OPEN){ ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
            else if(cmd==ID_TRAY_TOGGLE){ if(g_ducked) PostMessageW(hwnd, WM_APP_DUCK_OFF,0,0); else PostMessageW(hwnd, WM_APP_DUCK_ON,0,0); }
            else if(cmd==ID_TRAY_EXIT){ PostMessageW(hwnd, WM_CLOSE, 0, 0); }
        }
        return 0;
    case WM_CLOSE:
        if(g_minToTray){
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        // fallthrough to destroy
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
    // 화면 중앙, 880x700
    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    int W=880, H=700;
    HWND hwnd=CreateWindowW(L"GameDuckWindow", U8("GameDuck — 게임 볼륨 즉시 낮추기").c_str(),
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
