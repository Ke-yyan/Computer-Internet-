// client_win32.cpp — 聊天室客户端（Win32）：加入前必须填写昵称；界面微调
// 构建：g++ -std=c++17 client_win32.cpp -lws2_32 -luser32 -lgdi32 -municode -mwindows -o client.exe
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>   // 必须在 windows.h 之前
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstdio>

#pragma comment(lib, "Ws2_32.lib")

using std::string;
using std::wstring;
using std::thread;
using std::atomic;
using std::uint32_t;

const wchar_t* kClassName = L"ChatClientWin32";
enum { IDC_LBL_IP=1000, IDC_IP, IDC_LBL_PORT, IDC_PORT, IDC_LBL_NICK, IDC_NICK,
       IDC_BTNCONN, IDC_LOG, IDC_INPUT, IDC_SEND };
#define WM_APP_RX (WM_APP+1)

static HWND g_hLog, g_hInput, g_hBtnConn, g_hIp, g_hPort, g_hNick, g_hSend;
static HWND g_hLblIp, g_hLblPort, g_hLblNick;
static atomic<bool> g_connected{false};
static atomic<bool> g_exit{false};
static SOCKET g_sock = INVALID_SOCKET;
static thread g_rxThread;
static HFONT g_font = nullptr;
static HBRUSH g_brWhite = nullptr;

// UTF-8/UTF-16
static wstring utf8_to_wide(const string& s){
    if(s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),nullptr,0);
    wstring w(n,L'\0'); MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&w[0],n); return w;
}
static string wide_to_utf8(const wstring& w){
    if(w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),nullptr,0,nullptr,nullptr);
    string s(n,'\0'); WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),&s[0],n,nullptr,nullptr); return s;
}

// 发送/接收
static bool send_all(SOCKET s, const char* buf, size_t len){
    size_t sent=0; while(sent<len){ int n=send(s,buf+(int)sent,(int)(len-sent),0); if(n<=0) return false; sent+=n; } return true;
}
static bool recv_n(SOCKET s, char* buf, size_t len){
    size_t r=0; while(r<len){ int n=recv(s,buf+(int)r,(int)(len-r),0); if(n<=0) return false; r+=n; } return true;
}
static bool send_frame(SOCKET s, const string& payload){
    uint32_t L=htonl((uint32_t)payload.size()); return send_all(s,(char*)&L,4)&&send_all(s,payload.data(),payload.size());
}
static bool recv_frame(SOCKET s, string& out){
    uint32_t Lnet=0; if(!recv_n(s,(char*)&Lnet,4)) return false; uint32_t L=ntohl(Lnet);
    if(L>(1u<<20)) return false; out.assign(L,'\0'); return recv_n(s,out.data(),L);
}

// 极简 JSON
static string json_escape(const string& s){
    string o; o.reserve(s.size()+8);
    for(unsigned char c:s){ if(c=='\\'||c=='"'){o.push_back('\\');o.push_back((char)c);} else if(c=='\n') o+="\\n"; else o.push_back((char)c); }
    return o;
}
static string make_json(const string& type,const string& from,const string& text){
    return string("{\"type\":\"")+json_escape(type)+"\",\"from\":\""+json_escape(from)+"\",\"text\":\""+json_escape(text)+"\"}";
}
static string get_field(const string& j, const char* key){
    string pat=string("\"")+key+"\":\""; size_t p=j.find(pat); if(p==string::npos) return "";
    p+=pat.size(); string v; for(size_t i=p;i<j.size();++i){ char c=j[i]; if(c=='\\'){ if(i+1<j.size()){ v.push_back(j[i+1]); ++i; } }
        else if(c=='\"') break; else v.push_back(c); } return v;
}

static void SetFontAll(HWND h){
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
}
static void AppendLog(HWND h, const wstring& line){
    SendMessageW(h, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    wstring s=line+L"\r\n"; SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
}

static void RxLoop(HWND hWnd){
    string frame;
    while(g_connected && !g_exit){
        if(!recv_frame(g_sock, frame)){ PostMessageW(hWnd, WM_APP_RX, 0, (LPARAM)new wstring(L"连接被关闭")); break; }
        string t=get_field(frame,"type"), f=get_field(frame,"from"), tx=get_field(frame,"text");
        wstring line = (t=="msg") ? (utf8_to_wide(f)+L": "+utf8_to_wide(tx))
                                  : (utf8_to_wide(tx));   // 直接显示“xxx进入聊天区/离开”
        PostMessageW(hWnd, WM_APP_RX, 0, (LPARAM)new wstring(line));
    }
    g_connected=false;
}

static void EnableAddrBar(BOOL en){
    EnableWindow(g_hIp,en); EnableWindow(g_hPort,en); EnableWindow(g_hNick,en);
}

static void DoDisconnect(){
    if(!g_connected) return;
    g_connected=false; g_exit=true;
    int nlen=GetWindowTextLengthW(g_hNick); wstring w(nlen,L'\0'); GetWindowTextW(g_hNick,&w[0],nlen+1);
    string nick = wide_to_utf8(w.empty()?L"guest":w);
    send_frame(g_sock, make_json("quit", nick, ""));
    shutdown(g_sock, SD_BOTH); closesocket(g_sock); g_sock=INVALID_SOCKET;
    if(g_rxThread.joinable()) g_rxThread.join();
    SetWindowTextW(g_hBtnConn, L"加入"); EnableWindow(g_hSend, FALSE); EnableAddrBar(TRUE);
    AppendLog(g_hLog, L"你已离开聊天区");
}

static wstring Trim(const wstring& s){
    size_t i=0,j=s.size(); while(i<j && iswspace(s[i])) ++i; while(j>i && iswspace(s[j-1])) --j; return s.substr(i,j-i);
}

static void DoConnect(HWND hWnd){
    if(g_connected) return;
    wchar_t ipw[128], portw[16]; GetWindowTextW(g_hIp, ipw, 128); GetWindowTextW(g_hPort, portw, 16);
    int nlen=GetWindowTextLengthW(g_hNick); wstring nickw(nlen,L'\0'); GetWindowTextW(g_hNick,&nickw[0],nlen+1);
    wstring nickw_trim = Trim(nickw);
    if(nickw_trim.empty()){
        MessageBoxW(hWnd, L"请输入昵称后再加入。", L"提示", MB_OK|MB_ICONINFORMATION);
        SetFocus(g_hNick); SendMessageW(g_hNick, EM_SETSEL, 0, -1); return;
    }
    string host = wide_to_utf8(ipw); int port = _wtoi(portw);
    if(host.empty() || port<=0){ AppendLog(g_hLog, L"请输入正确的 IP 与端口"); return; }

    addrinfo hints{}; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    addrinfo* res=nullptr; char portStr[16]; sprintf(portStr, "%d", port);
    if(getaddrinfo(host.c_str(), portStr, &hints, &res)!=0){ AppendLog(g_hLog,L"解析地址失败"); return; }
    g_sock = socket(res->ai_family,res->ai_socktype,res->ai_protocol);
    if(g_sock==INVALID_SOCKET){ freeaddrinfo(res); AppendLog(g_hLog,L"socket 创建失败"); return; }
    if(connect(g_sock,res->ai_addr,(int)res->ai_addrlen)==SOCKET_ERROR){
        freeaddrinfo(res); closesocket(g_sock); g_sock=INVALID_SOCKET; AppendLog(g_hLog,L"连接失败"); return;
    }
    freeaddrinfo(res);

    g_connected=true; g_exit=false;
    string nick = wide_to_utf8(nickw_trim);
    send_frame(g_sock, make_json("join", nick, ""));  // 入场广播

    g_rxThread = thread(RxLoop, hWnd);
    SetWindowTextW(g_hBtnConn, L"离开"); EnableWindow(g_hSend, TRUE); EnableAddrBar(FALSE);
    AppendLog(g_hLog, L"你已加入聊天区");
}

static void DoSend(){
    if(!g_connected) return;
    int len = GetWindowTextLengthW(g_hInput); if(len<=0) return;
    wstring w(len,L'\0'); GetWindowTextW(g_hInput,&w[0],len+1);
    int nlen=GetWindowTextLengthW(g_hNick); wstring nw(nlen,L'\0'); GetWindowTextW(g_hNick,&nw[0],nlen+1);
    string nick = wide_to_utf8(nw.empty()?L"guest":nw);
    string text = wide_to_utf8(w);
    if(text == "/quit"){ DoDisconnect(); return; }
    if(!send_frame(g_sock, make_json("msg", nick, text))){ AppendLog(g_hLog,L"发送失败"); DoDisconnect(); return; }
    SetWindowTextW(g_hInput, L"");
}

static void DoLayout(HWND hWnd){
    RECT rc; GetClientRect(hWnd,&rc); int W=rc.right-rc.left, H=rc.bottom-rc.top;
    int pad=10,row=28,y=pad,x=pad;

    // 标签 + 输入框
    MoveWindow(g_hLblIp,  x, y+5, 70, row, TRUE); x+=70+6;
    MoveWindow(g_hIp,     x, y,   220, row, TRUE); x+=220+10;

    MoveWindow(g_hLblPort,x, y+5, 40, row, TRUE); x+=40+6;
    MoveWindow(g_hPort,   x, y,   80,  row, TRUE); x+=80+10;

    MoveWindow(g_hLblNick,x, y+5, 40, row, TRUE); x+=40+6;
    MoveWindow(g_hNick,   x, y,   140, row, TRUE); x+=140+10;

    MoveWindow(g_hBtnConn,x, y,   90,  row, TRUE);

    y+=row+pad; x=pad;
    MoveWindow(g_hLog,  x, y,   W-2*pad, H-y-row-2*pad, TRUE);
    MoveWindow(g_hInput,x, H-row-pad, W-2*pad-90-pad, row, TRUE);
    MoveWindow(g_hSend, W-pad-90, H-row-pad, 90, row, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam){
    switch(msg){
    case WM_CREATE:{
        // 字体与画刷
        g_font = CreateFontW(-16,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        g_brWhite = CreateSolidBrush(RGB(255,255,255));

        // 顶部栏
        g_hLblIp   = CreateWindowW(L"STATIC", L"服务器", WS_CHILD|WS_VISIBLE, 0,0,0,0, hWnd,(HMENU)IDC_LBL_IP,nullptr,nullptr);
        g_hIp      = CreateWindowW(L"EDIT",   L"127.0.0.1", WS_CHILD|WS_VISIBLE|WS_BORDER, 0,0,0,0, hWnd,(HMENU)IDC_IP,nullptr,nullptr);
        g_hLblPort = CreateWindowW(L"STATIC", L"端口",   WS_CHILD|WS_VISIBLE, 0,0,0,0, hWnd,(HMENU)IDC_LBL_PORT,nullptr,nullptr);
        g_hPort    = CreateWindowW(L"EDIT",   L"5000",     WS_CHILD|WS_VISIBLE|WS_BORDER, 0,0,0,0, hWnd,(HMENU)IDC_PORT,nullptr,nullptr);
        g_hLblNick = CreateWindowW(L"STATIC", L"昵称",   WS_CHILD|WS_VISIBLE, 0,0,0,0, hWnd,(HMENU)IDC_LBL_NICK,nullptr,nullptr);
        g_hNick    = CreateWindowW(L"EDIT",   L"",         WS_CHILD|WS_VISIBLE|WS_BORDER, 0,0,0,0, hWnd,(HMENU)IDC_NICK,nullptr,nullptr);
        g_hBtnConn = CreateWindowW(L"BUTTON", L"加入",     WS_CHILD|WS_VISIBLE, 0,0,0,0, hWnd,(HMENU)IDC_BTNCONN,nullptr,nullptr);

        // 聊天区与输入区
        g_hLog   = CreateWindowW(L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|ES_READONLY,
                                 0,0,0,0,hWnd,(HMENU)IDC_LOG,nullptr,nullptr);
        g_hInput = CreateWindowW(L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,0,0,0,0,hWnd,(HMENU)IDC_INPUT,nullptr,nullptr);
        g_hSend  = CreateWindowW(L"BUTTON", L"发送", WS_CHILD|WS_VISIBLE|WS_DISABLED,0,0,0,0,hWnd,(HMENU)IDC_SEND,nullptr,nullptr);

        // 统一字体
        SetFontAll(g_hLblIp); SetFontAll(g_hLblPort); SetFontAll(g_hLblNick);
        SetFontAll(g_hIp); SetFontAll(g_hPort); SetFontAll(g_hNick);
        SetFontAll(g_hBtnConn); SetFontAll(g_hLog); SetFontAll(g_hInput); SetFontAll(g_hSend);

        DoLayout(hWnd);
        SetFocus(g_hNick); SendMessageW(g_hNick, EM_SETSEL, 0, -1);
        break;
    }
    case WM_SIZE: DoLayout(hWnd); break;

    // 统一浅色背景
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:{
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, RGB(255,255,255));
        SetTextColor(hdc, RGB(40,40,40));
        return (LRESULT)g_brWhite;
    }

    case WM_COMMAND:{
        int id=LOWORD(wParam);
        if(id==IDC_BTNCONN){ if(g_connected) DoDisconnect(); else DoConnect(hWnd); }
        else if(id==IDC_SEND){ DoSend(); }
        break;
    }
    case WM_APP_RX:{ wstring* p=(wstring*)lParam; AppendLog(g_hLog,*p); delete p; break; }
    case WM_CLOSE: DoDisconnect(); DestroyWindow(hWnd); return 0;
    case WM_DESTROY:
        if(g_font) DeleteObject(g_font);
        if(g_brWhite) DeleteObject(g_brWhite);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}

int APIENTRY wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int nShow){
    WSADATA w; WSAStartup(MAKEWORD(2,2),&w);
    WNDCLASSW wc{}; wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName=kClassName;
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); RegisterClassW(&wc);
    HWND hWnd=CreateWindowW(kClassName,L"简易聊天室",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,920,620,nullptr,nullptr,hInst,nullptr);
    ShowWindow(hWnd,nShow);
    MSG msg; while(GetMessageW(&msg,nullptr,0,0)){
        if(msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN && GetFocus()==g_hInput){ DoSend(); continue; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    WSACleanup(); return 0;
}
