// main.cpp —— 命令行解析，调用发送端 / 接收端
#include "rudp.h"

#include <iostream>

int    g_linkDelayMs = 0;
double g_lossRate    = 0.0;
int    g_recvWindow  = DEFAULT_RECV_WINDOW;

static int clampWindowSize(int value)
{
    if (value < 1)
        return 1;
    if (value > 65535)
        return 65535;
    return value;
}

static void printUsage()
{
    std::cout << "Usage:\n"
              << "  rudp.exe recv <port> <output_file> [window_size]\n"
              << "  rudp.exe send <server_ip> <port> <input_file> [delay_ms] [loss_percent]\n";
}

int main(int argc, char* argv[])
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    if (argc < 2)
    {
        printUsage();
        WSACleanup();
        return 0;
    }

    std::string mode = argv[1];

    if (mode == "recv")
    {
        if (argc != 4 && argc != 5)
        {
            printUsage();
        }
        else
        {
            uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
            std::string outFile = argv[3];
            if (argc == 5)
            {
                g_recvWindow = clampWindowSize(std::stoi(argv[4]));
            }
            else
            {
                g_recvWindow = DEFAULT_RECV_WINDOW;
            }
            runReceiver(port, outFile);
        }
    }
    else if (mode == "send")
    {
        if (argc != 5 && argc != 7)
        {
            printUsage();
        }
        else
        {
            std::string ip   = argv[2];
            uint16_t    port = static_cast<uint16_t>(std::stoi(argv[3]));
            std::string file = argv[4];

            int    delayMs  = 0;
            double lossRate = 0.0;

            if (argc == 7)
            {
                delayMs  = std::stoi(argv[5]);
                lossRate = std::stod(argv[6]) / 100.0;
            }

            setLinkOptions(delayMs, lossRate);
            runSender(ip, port, file);
        }
    }
    else
    {
        printUsage();
    }

    WSACleanup();
    return 0;
}
