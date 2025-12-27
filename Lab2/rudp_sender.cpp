// rudp_sender.cpp —— 发送端：三次握手 + 滑动窗口 + Reno + SACK + 统计
#include "rudp.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <chrono>
#include <algorithm>
#include <cstring>

using Clock = std::chrono::steady_clock;

// 单个发送槽
struct SendSlot
{
    PacketHeader hdr{};
    std::vector<char> data;

    bool sent      = false;   // 是否发送过（任意一次）
    bool acked     = false;   // 是否已被确认
    bool firstSent = false;   // 是否已经记录 firstSendTime

    //用来计算RTT，ack收到时刻-firstSendTime
    Clock::time_point firstSendTime;
    //最近一次发送，可能是重传，用来判断是否需要重传
    //now-lastSendTime > timeout则需要重传
    Clock::time_point lastSendTime;
};

// ============ 三次握手（客户端） ============

static bool senderHandshake(SOCKET s, const sockaddr_in& serverAddr)
{
    int dynamicTimeout = HANDSHAKE_TIMEOUT_MS + 2 * g_linkDelayMs;  // 2倍链路延迟（往返）
    setRecvTimeout(s, dynamicTimeout); 
    //设置接收超时时间

    PacketHeader syn{};
    //构造一个只有SYN的包，序号从0开始
    syn.seq   = 0;
    syn.ack   = 0;
    syn.flags = FLAG_SYN;
    syn.wnd   = static_cast<uint16_t>(g_recvWindow);
    syn.reserved = 0;

    const int MAX_TRY = 5;//最多MAX_TRY次重试循环

    for (int i = 0; i < MAX_TRY; ++i)
    {
        std::cout << "[sender] send SYN\n";
        sendPacket(s, serverAddr, syn, nullptr, 0);

        PacketHeader resp{};
        std::vector<char> payload;
        sockaddr_in from{};

        if (recvPacket(s, resp, payload, from))
        {   //收到报文后进行判断标志位
            //判断是否为 SYN-ACK 报文
            bool isSynAck =
                ((resp.flags & (FLAG_SYN | FLAG_ACK)) ==
                 (FLAG_SYN | FLAG_ACK));
            //判断确认号，确认号＋1
            if (isSynAck && resp.ack == syn.seq + 1)
            {
                std::cout << "[sender] recv SYN-ACK\n";

                PacketHeader ack{};//构造最终ACK报文
                ack.seq   = syn.seq + 1;
                ack.ack   = resp.seq + 1;
                ack.flags = FLAG_ACK;
                ack.wnd   = static_cast<uint16_t>(g_recvWindow);
                ack.reserved = 0;
                //发送ack，并输出handshake success
                sendPacket(s, serverAddr, ack, nullptr, 0);
                std::cout << "[sender] handshake success\n";
                return true;
            }
        }
        //如果在这轮没有收到合法的SYN-ACK，则重发SYN
        std::cout << "[sender] handshake retry " << (i + 1) << "\n";
    }
    //最多尝试5次，如果5次都失败则握手失败，返回false
    std::cerr << "[sender] handshake failed\n";
    return false;
}

// ============ 四次挥手（客户端主动关闭） ============

static bool senderFourWayClose(SOCKET s, const sockaddr_in& serverAddr)
{
    setRecvTimeout(s, HANDSHAKE_TIMEOUT_MS);//设置接收超时时间

    PacketHeader fin1{};//构造第一次FIN报文
    fin1.seq   = 1;
    fin1.ack   = 0;
    fin1.flags = FLAG_FIN;
    fin1.wnd   = 0;
    fin1.reserved = 0;

    const int MAX_TRY = 5;

    for (int i = 0; i < MAX_TRY; ++i)
    {
        std::cout << "[sender] send FIN\n";//发送日志
         // sendPacket：调用公共工具函数，发送fin1包（已提前设置flags=FLAG_FIN、seq=1）
        sendPacket(s, serverAddr, fin1, nullptr, 0);

        // 等待 ACK（第二次挥手）
        PacketHeader resp{};// 存储接收端的响应头部
        std::vector<char> payload;// 存储响应的负载（ACK包无负载，仅占位）
        sockaddr_in from{}; // 存储响应发送方的地址（应等于接收端地址）
        //等待接收端ACK超时，重发
        if (!recvPacket(s, resp, payload, from))
        {
            std::cout << "[sender] FIN wait ACK timeout, retry\n";
            continue;
        }
        //收到接收端ACK
        if ((resp.flags & FLAG_ACK) && resp.ack == fin1.seq + 1)
        {
            std::cout << "[sender] recv ACK of FIN\n";

            // 等待对端 FIN（第三次挥手）
            PacketHeader peerFin{};
            std::vector<char> dummy;
            if (!recvPacket(s, peerFin, dummy, from))
            {
                std::cout << "[sender] wait peer FIN timeout, retry\n";
                continue; // 重发自己的 FIN
            }
            // 校验：接收端的响应是否是FIN包（四次挥手第3次的合法响应）
            if (peerFin.flags & FLAG_FIN)
            {
                std::cout << "[sender] recv peer FIN\n";

                // 发送最后一个 ACK（第四次挥手）
                PacketHeader ack2{};
                ack2.seq   = 0;
                ack2.ack   = peerFin.seq + 1;
                ack2.flags = FLAG_ACK;
                ack2.wnd   = static_cast<uint16_t>(g_recvWindow);
                ack2.reserved = 0;

                sendPacket(s, serverAddr, ack2, nullptr, 0);
                std::cout << "[sender] four-way close done\n";
                return true;
            }
        }
    }

    std::cerr << "[sender] four-way close failed\n";
    return false;
}

// ============ 发送端主逻辑 ============

void runSender(const std::string& ip, uint16_t port, const std::string& inputFile)
{
    //建立UDP Socket
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        printLastError("socket");
        return;
    }
    //解析地址
    sockaddr_in server{};
    server.sin_family      = AF_INET;
    server.sin_port        = htons(port);
    server.sin_addr.s_addr = inet_addr(ip.c_str());

    //三次握手
    if (!senderHandshake(s, server))
    {
        closesocket(s);
        return;
    }
    //打开待发送文件
    std::ifstream fin(inputFile, std::ios::binary);
    if (!fin)
    {
        std::cerr << "[sender] open input file failed\n";
        closesocket(s);
        return;
    }

    // 把文件切分成多个数据分组
    std::vector<SendSlot> slots;
    //每次最多读出MAX_PAYLOAD字节,构造一个SendSlot，放入slots
    char buffer[MAX_PAYLOAD];
    uint32_t seq = 1;                // 数据起始序号（与握手分开）

    while (true)
    {
        fin.read(buffer, MAX_PAYLOAD);
        std::streamsize n = fin.gcount();
        if (n <= 0)
            break;

        SendSlot slot;
        slot.hdr.seq   = seq++;
        slot.hdr.ack   = 0;
        slot.hdr.flags = FLAG_DATA;
        slot.hdr.wnd   = 0;
        slot.hdr.reserved = 0;
        slot.data.assign(buffer, buffer + n);
        slots.push_back(std::move(slot));
    }
    fin.close();
    //空文件处理
    if (slots.empty())
    {
        std::cout << "[sender] input file empty, nothing to send\n";
        senderFourWayClose(s, server);
        closesocket(s);
        return;
    }


    const uint32_t firstDataSeq = 1;//数据序号从1开始

    // 拥塞控制：简化 Reno
    double cwnd     = 1.0;   // 拥塞窗口（单位：分组）
    double ssthresh = 16.0;//慢启动阈值

    uint16_t peerWnd = static_cast<uint16_t>(g_recvWindow); // 对端通告窗口（初值）

    // Reno 所需的额外状态：用于实现快速重传 / 快速恢复
    uint32_t lastAckSeq   = firstDataSeq - 1; // 最近一次累计 ACK 的序号
    int      dupAckCount  = 0;                // 连续重复 ACK 次数
    bool     inFastRecovery = false;          // 是否处于快速恢复阶段
    uint32_t recoverSeq   = 0;                // 触发快速恢复时的“已发送最高序号”

    size_t base = 0;    // 当前窗口中最早未确认分组下标
    size_t next = 0;    // 下一个待发送分组下标

    // 统计信息
    bool started = false;
    Clock::time_point startTime, endTime;

    uint64_t bytesDelivered   = 0;   // 真实交付的字节数
    uint64_t totalPacketsSent = 0;   // DATA 总发送次数（含重传）
    uint64_t retransmissions  = 0;   // DATA 重传次数
    uint64_t rttSumUs        = 0;   // RTT 总和（微秒）
    uint64_t rttSamples       = 0;   // RTT 样本数

    setRecvTimeout(s, 10);   // 数据阶段：短超时轮询

    while (base < slots.size())//整个循环知道所有分组被确认为止
    {
        // 发送新的分组（窗口 = min（拥塞窗口cwnd, 对端通告窗口peerWnd, 未确认分组数））
        int windowLimit = static_cast<int>(
            std::min<double>(
                cwnd,
                std::min<double>(peerWnd,
                                 static_cast<double>(slots.size() - base))));

        while (next < slots.size() &&
               static_cast<int>(next - base) < windowLimit)
        {
            SendSlot& slot = slots[next];
            auto now = Clock::now();

            if (!slot.firstSent)
            {
                slot.firstSent   = true;
                slot.firstSendTime = now;

                if (!started)
                {
                    started  = true;
                    startTime = now;
                }
            }

            slot.lastSendTime = now;
            slot.sent         = true;

            if (!sendPacket(s, server,
                            slot.hdr,
                            slot.data.data(),
                            static_cast<uint16_t>(slot.data.size())))
            {
                // 发送出错直接退出
                closesocket(s);
                return;
            }

            ++totalPacketsSent;
            ++next;
        }

        // 接收 ACK + SACK
        PacketHeader ackHdr{};
        std::vector<char> ackPayload;
        sockaddr_in from{};

        if (recvPacket(s, ackHdr, ackPayload, from))//成功接收ACK
        {
            if (ackHdr.flags & FLAG_ACK) // 仅处理 ACK 类型的包
            {   // 更新接收端通告窗口（0 时设为 1，避免窗口为 0 导致阻塞）
                //防止对端通告为0的时候直接卡住
                peerWnd = (ackHdr.wnd == 0 ? 1 : ackHdr.wnd);

                bool anyNewAck = false;
                auto now = Clock::now();
                // 处理累计 ACK（Reno 部分）
                if (ackHdr.ack >= firstDataSeq)//ACK前进
                {
                    if (ackHdr.ack > lastAckSeq)
                    {
                        lastAckSeq  = ackHdr.ack;
                        dupAckCount = 0;

                        // 累计 ACK 前进，若此前在快速恢复则在超过 recover 点时退出
                        if (inFastRecovery && ackHdr.ack > recoverSeq)
                        {
                            inFastRecovery = false;
                            cwnd           = ssthresh;
                            if (cwnd > 64.0)
                                cwnd = 64.0;
                        }
                    }//ACK未前进
                    else if (ackHdr.ack == lastAckSeq)
                    {
                        ++dupAckCount;

                        // 进入快速重传：累计 ACK 重复 3 次且仍有未确认分组
                        if (!inFastRecovery && dupAckCount >= 3 && base < slots.size())
                        {
                            size_t lossIdx = base;
                            if (lossIdx < slots.size() &&
                                slots[lossIdx].sent && !slots[lossIdx].acked)
                            {
                                // 退避并设置窗口到 ssthresh+3，立即重传推测丢失的分组
                                ssthresh = (cwnd / 2.0 < 2.0) ? 2.0 : (cwnd / 2.0);
                                cwnd     = ssthresh + 3.0;
                                if (cwnd > 64.0)
                                    cwnd = 64.0;
                                inFastRecovery = true;
                                if (next > 0)
                                    recoverSeq = slots[next - 1].hdr.seq;
                                else
                                    recoverSeq = slots[lossIdx].hdr.seq;

                                slots[lossIdx].lastSendTime = now;
                                if (!sendPacket(s, server,
                                                slots[lossIdx].hdr,
                                                slots[lossIdx].data.data(),
                                                static_cast<uint16_t>(slots[lossIdx].data.size())))
                                {
                                    closesocket(s);
                                    return;
                                }

                                ++totalPacketsSent;
                                ++retransmissions;
                            }
                        }
                        else if (inFastRecovery)
                        {
                            // 快速恢复阶段：每个重复 ACK 线性增大 cwnd
                            cwnd += 1.0;
                            if (cwnd > 64.0)
                                cwnd = 64.0;
                        }
                    }
                    else
                    {
                        dupAckCount = 0;
                    }
                }

                //工具函数：标记某个分组已被确认
                auto markIndexAcked = [&](size_t idx)
                {
                    if (idx >= slots.size())
                        return;
                    SendSlot& slot = slots[idx];
                    if (!slot.acked)
                    {
                        slot.acked = true;
                        anyNewAck  = true;
                        //记录成功交付字节数
                        bytesDelivered += slot.data.size();

                        if (slot.firstSent)
                        {
                            // 以微秒为单位统计 RTT
                            auto rttUs =
                                std::chrono::duration_cast<
                                    std::chrono::microseconds>(
                                    now - slot.firstSendTime).count();

                            if (rttUs > 0)
                            {
                                rttSumUs += static_cast<uint64_t>(rttUs);
                                ++rttSamples;
                            }
                        }
                    }

                };

                // 累计确认：从 firstDataSeq 到 ackHdr.ack
                //ackHdr.ack表示这个序号之前的所有分组都已经收到
                if (ackHdr.ack >= firstDataSeq)
                {
                    //映射成下标，逐个调用MarkIndexAcked
                    uint32_t maxAckSeq =
                        std::min<uint32_t>(
                            ackHdr.ack,
                            firstDataSeq + static_cast<uint32_t>(slots.size()) - 1);

                    for (uint32_t seqNum = firstDataSeq;
                         seqNum <= maxAckSeq; ++seqNum)
                    {
                        size_t idx = static_cast<size_t>(seqNum - firstDataSeq);
                        markIndexAcked(idx);
                    }
                }

                // 解析 SACK block（选择确认）
                if (ackPayload.size() >= sizeof(uint16_t))
                {
                    uint16_t blkCount = 0;
                    std::memcpy(&blkCount, ackPayload.data(), sizeof(uint16_t));
                    blkCount = std::min<uint16_t>(blkCount, MAX_SACK_BLOCKS);

                    size_t offset = sizeof(uint16_t);
                    for (uint16_t i = 0; i < blkCount; ++i)
                    {
                        if (offset + sizeof(SackBlock) > ackPayload.size())
                            break;

                        SackBlock blk{};
                        std::memcpy(&blk,
                                    ackPayload.data() + offset,
                                    sizeof(SackBlock));
                        offset += sizeof(SackBlock);

                        uint32_t start = std::max<uint32_t>(
                            blk.start, firstDataSeq);
                        uint32_t end   = std::min<uint32_t>(
                            blk.end,
                            firstDataSeq +
                                static_cast<uint32_t>(slots.size()) - 1);

                        if (end < start)
                            continue;

                        for (uint32_t seqNum = start; seqNum <= end; ++seqNum)
                        {
                            size_t idx =
                                static_cast<size_t>(seqNum - firstDataSeq);
                            markIndexAcked(idx);
                        }
                    }
                }
                //只有在本轮真的有新的分组被确认时，才进行窗口前移
                if (anyNewAck)
                {
                    // 窗口前移
                    //从当前base开始，只要连续的槽都被确认了，就把Base往右移
                    while (base < slots.size() && slots[base].acked)
                        ++base;

                    // Reno 拥塞控制
                    if (cwnd < ssthresh)
                    {
                        cwnd += 1.0;                 // 慢启动
                    }
                    else
                    {
                        cwnd += 1.0 / cwnd;          // 拥塞避免
                    }
                    if (cwnd > 64.0)
                        cwnd = 64.0;
                }
            }
        }

        // 检查超时重传
        auto now = Clock::now();
        //只检查当前窗口中，已经发出去但还没有全部ACK的那一段的分组
        for (size_t i = base; i < next; ++i)
        {
            SendSlot& slot = slots[i];
            if (!slot.sent || slot.acked)
                continue;
            //计算距离上次发送过去经过的时间，即飞行的时长
            auto elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(now - slot.lastSendTime).count();

            if (elapsed > g_dataTimeoutMs)
            {
                /*std::cout << "[sender] timeout seq=" << slot.hdr.seq
                        << " (elapsed=" << elapsed
                        << "ms, RTO=" << g_dataTimeoutMs << "ms)\n";*/

                // 重传，先更新时间
                slot.lastSendTime = now;

                if (!sendPacket(s, server,
                                slot.hdr,
                                slot.data.data(),
                                static_cast<uint16_t>(slot.data.size())))
                {
                    closesocket(s);
                    return;
                }

                ++totalPacketsSent;
                ++retransmissions;//总发送次数+1，重传次数+1

                // 拥塞控制 —— 认为发生拥塞，退回慢启动
                ssthresh = (cwnd / 2.0 < 2.0) ? 2.0 : (cwnd / 2.0);
                cwnd     = ssthresh;
                //窗口减半，退回到慢启动//拥塞避免交界点,减小发送速率，重新探测带宽
            }
        }
    }

    endTime = Clock::now();

    // 主动发起四次挥手
    senderFourWayClose(s, server);
    closesocket(s);

    // 统计结果
    double durationSec =
        std::chrono::duration<double>(endTime - startTime).count();
    if (durationSec <= 0.0)
        durationSec = 1e-6;

    double throughputMBps =
        static_cast<double>(bytesDelivered) / durationSec / (1024.0 * 1024.0);
    double throughputMbps = throughputMBps * 8.0;

    double lossRate =
        (totalPacketsSent > 0)
            ? static_cast<double>(retransmissions) /
                  static_cast<double>(totalPacketsSent)
            : 0.0;

    double avgRttUs =
        (rttSamples > 0)
            ? static_cast<double>(rttSumUs) /
                  static_cast<double>(rttSamples)
            : 0.0;

    std::cout << "===== RUDP Statistics (Sender) =====\n";
    std::cout << "Bytes delivered:       " << bytesDelivered << " bytes\n";
    std::cout << "Data packets sent:     " << totalPacketsSent
              << " (retransmissions=" << retransmissions << ")\n";
    std::cout << "Approx. loss rate:     " << lossRate * 100.0 << " %\n";
    std::cout << "Average RTT:           " << avgRttUs << " us\n";
    std::cout << "Throughput:            " << throughputMBps
              << " MB/s (" << throughputMbps << " Mbps)\n";

    std::cout << "Configured recv window: " << g_recvWindow << " packets\n";
}
