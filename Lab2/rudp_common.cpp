// rudp_common.cpp —— 公共工具函数实现
#include "rudp.h"

#include <iostream>
#include <cstring>

#include <random>
#include <thread>
#include <chrono>

int g_dataTimeoutMs = TIMEOUT_MS;  // 默认就用原来的常数

//设置 Socket 接收超时
void setRecvTimeout(SOCKET s, int ms)
{
    int opt = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&opt), sizeof(opt));
}


//打印网络错误信息
void printLastError(const char* where)
{
    std::cerr << "[" << where << "] WSA error: "
              << WSAGetLastError() << std::endl;
}


// 16 位互联网校验和
uint16_t checksum16(const char* data, size_t len)
{
    uint32_t sum = 0;
    size_t i = 0;

    while (i + 1 < len)
    {
        uint16_t word =
            (static_cast<uint8_t>(data[i])   << 8) |
             static_cast<uint8_t>(data[i+1]);
        sum += word;
        if (sum & 0x10000)                 // 产生进位则回卷
            sum = (sum & 0xFFFF) + 1;
        i += 2;
    }

    if (i < len)                            // 奇数字节
    {
        uint16_t word = static_cast<uint8_t>(data[i]) << 8;
        sum += word;
        if (sum & 0x10000)
            sum = (sum & 0xFFFF) + 1;
    }
    //按位取反得到校验值
    return static_cast<uint16_t>(~sum);
}


bool sendPacket(
    SOCKET s,
    const sockaddr_in& addr,
    PacketHeader hdr,
    const char* payload,
    uint16_t payloadLen)
{
    // ===== 模拟丢包 / 延迟的随机数 =====
    //定义线程本地的随机数引擎rng,保证每个线程有独立的随机数序列
    static thread_local std::mt19937 rng{std::random_device{}()};

    bool isPureAck = (hdr.flags & FLAG_ACK) && !(hdr.flags & FLAG_DATA)
                     && !(hdr.flags & FLAG_SYN) && !(hdr.flags & FLAG_FIN);

    // 把真正 send 的逻辑封装成一个小函数，方便复用
    auto do_real_send = [&]() -> bool {
        hdr.len      = payloadLen;
        hdr.checksum = 0;

        //总长度
        const size_t totalLen = sizeof(PacketHeader) + payloadLen;
        std::vector<char> buffer(totalLen);

        std::memcpy(buffer.data(), &hdr, sizeof(PacketHeader));
        if (payloadLen > 0 && payload != nullptr)
        {
            //拷贝负载数据
            std::memcpy(buffer.data() + sizeof(PacketHeader),
                        payload, payloadLen);
        }

        // 计算校验和
        hdr.checksum = checksum16(buffer.data(), buffer.size());
        std::memcpy(buffer.data(), &hdr, sizeof(PacketHeader));
        //发送数据报
        int ret = sendto(
            s,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<const sockaddr*>(&addr),
            sizeof(addr));

        if (ret == SOCKET_ERROR)
        {
            printLastError("sendto");
            return false;
        }
        return true;
    };

    // ================= 纯 ACK：不丢包，不延迟 =================
    if (isPureAck)
    {
        return do_real_send();
    }

    // ================= 其他包：按设置丢包 + 延迟 =================
    //每次调用dist(rng)生成一个[0.0,1.0)之间的均匀分布随机数
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    if (dist(rng) < g_lossRate)
    {
        // 丢包：什么都不发，直接返回 true
        return true;
    }

    //人为制造发送延迟，模拟网络链路时延
    if (g_linkDelayMs > 0)
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(g_linkDelayMs));
    }

    return do_real_send();
}




bool recvPacket(
    SOCKET s,
    PacketHeader& hdr,
    std::vector<char>& payload,
    sockaddr_in& from)
{
    //准备缓冲区和来源地址长度
    char buffer[sizeof(PacketHeader) + MAX_PAYLOAD];
    int  fromLen = sizeof(from);

    //长度为ret
    int ret = recvfrom(
        s,
        buffer,
        sizeof(buffer),
        0,
        reinterpret_cast<sockaddr*>(&from),
        &fromLen);

    if (ret == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK)
        {
            // 超时：由调用者决定是否重试
            return false;
        }
        printLastError("recvfrom");
        return false;
    }
    //如果一个完整的头部都没有收到，则报文无效
    if (ret < static_cast<int>(sizeof(PacketHeader)))
    {
        std::cerr << "[recvPacket] packet too short\n";
        return false;
    }

    // 拷贝头部
    PacketHeader wireHdr{};
    std::memcpy(&wireHdr, buffer, sizeof(PacketHeader));
    uint16_t recvChecksum = wireHdr.checksum;

    // 重新计算校验和
    reinterpret_cast<PacketHeader*>(buffer)->checksum = 0;
    uint16_t calcChecksum = checksum16(buffer, ret);
    //如果重新计算出来的和原本的一样，则没有错误
    if (recvChecksum != calcChecksum)
    {
        std::cerr << "[recvPacket] checksum error\n";
        return false;
    }

    hdr = wireHdr;
    //计算数据部分长度
    int payloadLen = ret - static_cast<int>(sizeof(PacketHeader));
    payload.assign(buffer + sizeof(PacketHeader),
                   buffer + sizeof(PacketHeader) + payloadLen);
    return true;
}


void setLinkOptions(int delayMs, double lossRate)
{
    if (delayMs < 0) delayMs = 0;
    if (lossRate < 0) lossRate = 0;
    if (lossRate > 1) lossRate = 1;
    g_linkDelayMs = delayMs;
    g_lossRate    = lossRate;

    // 简单粗暴：数据阶段一律 300ms 超时，足够覆盖轮询 + 模拟延迟
    g_dataTimeoutMs = 300;

    std::cout << "[opts] delay=" << g_linkDelayMs
              << "ms, loss=" << (g_lossRate * 100.0)
              << "%, dataTimeout=" << g_dataTimeoutMs << "ms\n";
}

