// server.cpp — 简易聊天室服务器：4B长度前缀 + UTF-8 JSON {type,from,text}
// 构建：g++ -std=c++17 server.cpp -lws2_32 -o server.exe
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

using std::string;
using std::vector;
using std::thread;
using std::mutex;
using std::lock_guard;
using std::atomic;
using std::size_t;
using std::uint32_t;

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  using socket_t = SOCKET;
  static inline void net_init(){ WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
  static inline void net_cleanup(){ WSACleanup(); }
  static inline void closesock(socket_t s){ closesocket(s); }
  static inline int  last_net_err(){ return WSAGetLastError(); }
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  using socket_t = int;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  static inline void net_init(){}
  static inline void net_cleanup(){}
  static inline void closesock(socket_t s){ close(s); }
  static inline int  last_net_err(){ return errno; }
#endif

// 发送/接收固定字节
static bool send_all(socket_t s, const char* buf, size_t len){
    size_t sent = 0;
    while(sent < len){
        int n = send(s, buf + sent, (int)(len - sent), 0);
        if(n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}
static bool recv_n(socket_t s, char* buf, size_t len){
    size_t recvd = 0;
    while(recvd < len){
        int n = recv(s, buf + recvd, (int)(len - recvd), 0);
        if(n <= 0) return false;
        recvd += (size_t)n;
    }
    return true;
}

// 分帧
static bool send_frame(socket_t s, const string& payload){
    uint32_t L = htonl((uint32_t)payload.size());
    return send_all(s, (char*)&L, 4) && send_all(s, payload.data(), payload.size());
}
static bool recv_frame(socket_t s, string& out){
    uint32_t Lnet=0;
    if(!recv_n(s, (char*)&Lnet, 4)) return false;
    uint32_t L = ntohl(Lnet);
    if(L > (1u<<20)) return false; // 1MB 上限
    out.assign(L, '\0');
    return recv_n(s, out.data(), L);
}

// 极简 JSON
static string json_escape(const string& s){
    string o; o.reserve(s.size()+8);
    for(unsigned char c : s){
        if(c=='\\' || c=='"'){ o.push_back('\\'); o.push_back((char)c); }
        else if(c=='\n'){ o += "\\n"; }
        else o.push_back((char)c);
    }
    return o;
}
static string make_json(const string& type,const string& from,const string& text){
    return string("{\"type\":\"")+json_escape(type)+"\",\"from\":\""+json_escape(from)+"\",\"text\":\""+json_escape(text)+"\"}";
}
static string get_field(const string& j, const char* key){
    string pat = string("\"")+key+"\":\"";
    size_t p = j.find(pat);
    if(p == string::npos) return "";
    p += pat.size();
    string v;
    for(size_t i=p;i<j.size();++i){
        char c = j[i];
        if(c=='\\'){ if(i+1<j.size()){ v.push_back(j[i+1]); ++i; } }
        else if(c=='"'){ break; }
        else v.push_back(c);
    }
    return v;
}

// 全局状态
struct ClientInfo{ socket_t sock; string nick; };
static mutex g_mu;
static vector<ClientInfo> g_clients;
static atomic<bool> g_running{true};

static void broadcast(const string& json){
    lock_guard<mutex> lk(g_mu);
    for(auto it = g_clients.begin(); it != g_clients.end(); ){
        if(!send_frame(it->sock, json)){ closesock(it->sock); it = g_clients.erase(it); }
        else ++it;
    }
}

static void handle_client(socket_t cs){
    string first;
    if(!recv_frame(cs, first)){ closesock(cs); return; }
    string type = get_field(first, "type");
    string nick = get_field(first, "from");
    if(type != "join" || nick.empty()) nick = "guest";

    { lock_guard<mutex> lk(g_mu); g_clients.push_back({cs, nick}); }
    broadcast(make_json("sys", "server", nick + "进入聊天区"));

    string frame;
    while(g_running){
        if(!recv_frame(cs, frame)) break;
        string t = get_field(frame, "type");
        string txt = get_field(frame, "text");
        if(t=="msg") broadcast(make_json("msg", nick, txt));
        else if(t=="quit") break;
    }

    { lock_guard<mutex> lk(g_mu);
      for(auto it=g_clients.begin(); it!=g_clients.end(); ++it){ if(it->sock==cs){ g_clients.erase(it); break; } }
    }
    broadcast(make_json("sys", "server", nick + "离开聊天区"));
    closesock(cs);
}

int main(int argc, char** argv){
    int port = (argc>=2) ? atoi(argv[1]) : 5000;

    net_init();

    socket_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if(ls == INVALID_SOCKET){ fprintf(stderr,"socket() failed\n"); return 1; }

    int yes = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    sockaddr_in addr{}; addr.sin_family=AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if(bind(ls, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR){
        fprintf(stderr,"bind() failed: %d\n", last_net_err()); closesock(ls); net_cleanup(); return 1;
    }
    if(listen(ls, 16) == SOCKET_ERROR){
        fprintf(stderr,"listen() failed\n"); closesock(ls); net_cleanup(); return 1;
    }

    printf("server listening on 0.0.0.0:%d\n", port);

    while(g_running){
        sockaddr_in cli{}; socklen_t cl = sizeof(cli);
        socket_t cs = accept(ls, (sockaddr*)&cli, &cl);
        if(cs == INVALID_SOCKET){ fprintf(stderr,"accept() failed\n"); break; }
        thread(handle_client, cs).detach();
    }

    closesock(ls); net_cleanup(); return 0;
}
