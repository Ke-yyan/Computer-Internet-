// rudp_receiver.cpp —— 接收端：三次握手 + SACK + 四次挥手
#include "rudp.h"

#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <algorithm>
#include <cstring>

// ============ 三次握手（服务端） ============

static bool receiverHandshake(SOCKET s, sockaddr_in& clientAddr)
{
    setRecvTimeout(s, 0); // 阻塞等待 SYN

    PacketHeader syn{};
    std::vector<char> dummy;

    std::cout << "[receiver] wait for SYN...\n";
    //循环调用 recvPacket 直到收到 SYN 报文
    while (true)
    {
        if (!recvPacket(s, syn, dummy, clientAddr))
            continue;

        if (syn.flags & FLAG_SYN)
            break;
    }

    std::cout << "[receiver] recv SYN\n";

    PacketHeader synAck{};
    synAck.seq   = 100;            // 服务端自己的初始序号（随便选）
    synAck.ack   = syn.seq + 1;
    synAck.flags = FLAG_SYN | FLAG_ACK;//flag同时带有SYN和ACK
    synAck.wnd   = static_cast<uint16_t>(g_recvWindow);
    synAck.reserved = 0;

    std::cout << "[receiver] send SYN-ACK\n";
    sendPacket(s, clientAddr, synAck, nullptr, 0);

    // 等最后一个 ACK
    int dynamicHandshakeTimeout = HANDSHAKE_TIMEOUT_MS + 2 * g_linkDelayMs;
    setRecvTimeout(s, dynamicHandshakeTimeout);
    
    const int MAX_TRY = 5;

    for (int i = 0; i < MAX_TRY; ++i)
    {
        PacketHeader last{};
        std::vector<char> dummy2;
        sockaddr_in from{};
        if (!recvPacket(s, last, dummy2, from))
        {
            // 超时则重发 SYN-ACK
            std::cout << "[receiver] wait ACK timeout, resend SYN-ACK\n";
            sendPacket(s, clientAddr, synAck, nullptr, 0);
            continue;
        }

        if ((last.flags & FLAG_ACK) && last.ack == synAck.seq + 1)
        {
            std::cout << "[receiver] handshake success\n";
            setRecvTimeout(s, 0); // 数据阶段改回阻塞
            return true;
        }
    }

    std::cerr << "[receiver] handshake failed\n";
    return false;
}

// 构造 ACK + SACK payload 并发送
//buffer是一个有序的map(seq,data)，存的是已经收到了，但还没有按序写入文件的乱序分组
//cumulativeAck是已经按序收到并写入文件的最大序号
static void sendAckWithSack(
    SOCKET s,
    const sockaddr_in& clientAddr,
    uint32_t cumulativeAck,
    const std::map<uint32_t, std::vector<char>>& buffer)
{
    // 根据 buffer 里的乱序分组构造多个区间
    std::vector<SackBlock> blocks;
    uint32_t lastStart = 0, lastEnd = 0;
    bool hasRange = false;
    //在buffer中按照序号，从小到大把连续的序号合并成区间
    for (const auto& kv : buffer)
    {
        uint32_t seq = kv.first;
        if (seq <= cumulativeAck)
            continue;

        if (!hasRange)
        {
            lastStart = lastEnd = seq;
            hasRange  = true;
        }
        else if (seq == lastEnd + 1)
        {
            lastEnd = seq;
        }
        //把之前的乱序区间作为一个SackBlock放入blocks，开始新的区间
        else
        {
            blocks.push_back(SackBlock{lastStart, lastEnd});
            if (blocks.size() >= MAX_SACK_BLOCKS)
                break;
            lastStart = lastEnd = seq;
        }
    }
    if (hasRange && blocks.size() < MAX_SACK_BLOCKS)
    {
        blocks.push_back(SackBlock{lastStart, lastEnd});
    }

    uint16_t blkCount = static_cast<uint16_t>(blocks.size());

    // payload 格式：
    // [uint16_t blkCount][SackBlock blk1][SackBlock blk2]...
    std::vector<char> payload;
    payload.resize(sizeof(uint16_t) + blkCount * sizeof(SackBlock));
    std::memcpy(payload.data(), &blkCount, sizeof(uint16_t));
    size_t offset = sizeof(uint16_t);
    for (size_t i = 0; i < blocks.size(); ++i)
    {
        std::memcpy(payload.data() + offset,
                    &blocks[i], sizeof(SackBlock));
        offset += sizeof(SackBlock);
    }

    PacketHeader ackHdr{};
    ackHdr.seq   = 0;
    ackHdr.ack   = cumulativeAck;
    ackHdr.flags = FLAG_ACK;
    // 简单流量控制：窗口 = g_recvWindow - 当前缓存的分组数
    // 已经缓存的乱序分组数量buffer.size()，可用窗口就是 g_recvWindow 减去这个数量
    uint16_t avail = static_cast<uint16_t>(
        std::max<int>(1, g_recvWindow -
                         static_cast<int>(buffer.size())));
    ackHdr.wnd   = avail;
    ackHdr.reserved = 0;

    sendPacket(s, clientAddr,
               ackHdr,
               payload.data(),
               static_cast<uint16_t>(payload.size()));
}

// ============ 接收端主逻辑 ============

void runReceiver(uint16_t port, const std::string& outputFile)
{
    //创建UDP套接字并绑定端口
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        printLastError("socket");
        return;
    }

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons(port);
    //绑定端口
    if (bind(s, reinterpret_cast<sockaddr*>(&local),
             sizeof(local)) == SOCKET_ERROR)
    {
        printLastError("bind");
        closesocket(s);
        return;
    }
    //三次握手，确认对端地址
    sockaddr_in clientAddr{};
    if (!receiverHandshake(s, clientAddr))
    {
        closesocket(s);
        return;
    }

    std::ofstream fout(outputFile, std::ios::binary);
    if (!fout)
    {
        std::cerr << "[receiver] open output file failed\n";
        closesocket(s);
        return;
    }

    uint32_t expectedSeq = 1; // 期望的下一个有序分组号
    std::map<uint32_t, std::vector<char>> buffer; // 用一个有序map做乱序缓存
    //key表示分组的序号，value表示这个分组的payload数据

    bool finReceived = false;//标记是否已经收到了对方的FIN
    uint32_t finSeq  = 0;//记录对方的FIN包的序号，用于后续的ACK确认

    //没收到FIN就一直循环
    while (!finReceived)
    {
        PacketHeader hdr{};
        std::vector<char> data;
        sockaddr_in from{};

        if (!recvPacket(s, hdr, data, from))
            continue;
        //处理数据报文
        if (hdr.flags & FLAG_DATA)
        {
            // 收到数据分组
            if (hdr.seq >= expectedSeq)
            {
                // 只缓存之前没收到的 seq
                //收到乱序数据先放在map里，等缺失的前面分组到了一起写
                if (buffer.find(hdr.seq) == buffer.end())
                {
                    buffer[hdr.seq] = data;
                }

                // 把连续有序的分组写入文件
                while (true)
                {
                    auto it = buffer.find(expectedSeq);
                    if (it == buffer.end())
                        break;
                    //从map中取出对应序号的分组数据写入文件
                    fout.write(it->second.data(),
                               static_cast<std::streamsize>(
                                   it->second.size()));
                    buffer.erase(it);//从buffer中删除该分组
                    ++expectedSeq;
                }
                //这样就实现了一旦前边的窗口补齐，就可以把后面已经缓存好的连续段一次写出来
            }
            //累计确认号，已经成功按序收到并写入文件的最大序号
            uint32_t cumulativeAck = expectedSeq - 1;
            //payload中带SACK信息，把buffer中所有比cumulativeAck大的分组区间都带上
            sendAckWithSack(s, clientAddr, cumulativeAck, buffer);
        }
        else if (hdr.flags & FLAG_FIN)
        {
            std::cout << "[receiver] recv FIN\n";
            finReceived = true;//退出循环
            finSeq      = hdr.seq;
        }
    }

    fout.close();

    // ===== 四次挥手（服务端被动关闭） =====
    // 已经完成：
    //   (1) 客户端 ---> FIN
    // 现在要完成：
    //   (2) 服务端 ---> ACK
    //   (3) 服务端 ---> FIN
    //   (4) 客户端 ---> ACK

    // (2) ACK 客户端 FIN
    PacketHeader ack1{};
    ack1.seq   = 0;
    ack1.ack   = finSeq + 1;
    ack1.flags = FLAG_ACK;
    ack1.wnd   = static_cast<uint16_t>(g_recvWindow);
    ack1.reserved = 0;

    sendPacket(s, clientAddr, ack1, nullptr, 0);
    std::cout << "[receiver] send ACK of FIN\n";

    // (3) 发送自己的 FIN
    PacketHeader fin2{};
    fin2.seq   = 2;
    fin2.ack   = 0;
    fin2.flags = FLAG_FIN;
    fin2.wnd   = 0;
    fin2.reserved = 0;

    setRecvTimeout(s, HANDSHAKE_TIMEOUT_MS);
    const int MAX_TRY = 5;

    for (int i = 0; i < MAX_TRY; ++i)
    {
        std::cout << "[receiver] send FIN\n";
        sendPacket(s, clientAddr, fin2, nullptr, 0);

        // 等待客户端最后的 ACK
        PacketHeader resp{};
        std::vector<char> payload;
        sockaddr_in from{};
        if (!recvPacket(s, resp, payload, from))
        {
            std::cout << "[receiver] wait last ACK timeout\n";
            continue;
        }

        if ((resp.flags & FLAG_ACK) && resp.ack == fin2.seq + 1)
        {
            std::cout << "[receiver] four-way close done\n";
            break;
        }
    }

    closesocket(s);
}
