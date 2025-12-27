// rudp.h  —— 公共协议定义 + 函数声明
#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define NOMINMAX
#include <cstdint>
#include <vector>
#include <string>
//======== 协议参数 =======================

inline constexpr int MAX_PAYLOAD            = 1000;   // 每个数据分组最大负载
inline constexpr int DEFAULT_RECV_WINDOW   = 64;     // 默认接收窗口大小（流量控制）
inline constexpr int TIMEOUT_MS            = 100;    // 数据分组超时时间
inline constexpr int HANDSHAKE_TIMEOUT_MS  = 1000;   // 握手 / 挥手阶段超时时间
inline constexpr int MAX_SACK_BLOCKS       = 4;      // 一次 ACK 携带的最大区间数

extern int g_dataTimeoutMs;  // 运行时使用的“数据超时”
extern int g_recvWindow;     // 运行时接收窗口大小
// 标志位
enum PacketFlags : uint8_t
{
    FLAG_SYN  = 0x01,
    FLAG_ACK  = 0x02,
    FLAG_FIN  = 0x04,
    FLAG_DATA = 0x08
};

// 分组头部（16 字节）
#pragma pack(push, 1)
struct PacketHeader
{
    uint32_t seq;       // 序号（对 DATA 有效）
    uint32_t ack;       // 确认号（对 ACK 有效：累计确认）
    uint16_t len;       // 负载长度
    uint16_t wnd;       // 接收窗口通告
    uint16_t checksum;  // 16 位校验和（头 + 数据）
    uint8_t  flags;     // 标志位
    uint8_t  reserved;  // 对齐用，置 0，保留位
};
#pragma pack(pop)

// SACK 区间 [start, end]（包含端点）
struct SackBlock
{
    uint32_t start;
    uint32_t end;
};

// ======================= 公共工具函数 =======================

void setRecvTimeout(SOCKET s, int ms);
void printLastError(const char* where);

// 16 位互联网校验和
uint16_t checksum16(const char* data, size_t len);

// 发送一个分组（负责填充 hdr.len / hdr.checksum）
bool sendPacket(
    SOCKET s,
    const sockaddr_in& addr,
    PacketHeader hdr,
    const char* payload,
    uint16_t payloadLen);

// 接收一个分组并校验，成功返回 true，失败/超时返回 false
bool recvPacket(
    SOCKET s,
    PacketHeader& hdr,
    std::vector<char>& payload,
    sockaddr_in& from);

// ======================= 发送端 / 接收端接口 =======================

void runSender(const std::string& ip, uint16_t port, const std::string& inputFile);
void runReceiver(uint16_t port, const std::string& outputFile);


//设置丢包率和延迟时间
extern int    g_linkDelayMs;   // 模拟链路单向延迟（毫秒）
extern double g_lossRate;      // 模拟丢包率 [0,1]

void setLinkOptions(int delayMs, double lossRate);
