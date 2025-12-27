# Lab2：可靠 UDP (RUDP) 发送端/接收端

本实验基于 Windows 平台的 WinSock2，实现了一个带三次握手、四次挥手、滑动窗口、Reno 拥塞控制和 SACK 的可靠 UDP 传输。

## 一、编译方式

使用 Visual Studio 开发者命令行 (Developer Command Prompt for VS)，进入 `Lab2` 目录后执行：

```bat
cl /EHsc /std:c++17 /utf-8 main.cpp rudp_common.cpp rudp_sender.cpp rudp_receiver.cpp ws2_32.lib /Fe:rudp.exe
```

说明：
- `/EHsc`：启用标准 C++ 异常处理。
- `/std:c++17`：使用 C++17 标准。
- `/utf-8`：源文件按 UTF-8 编码编译。
- `ws2_32.lib`：链接 WinSock2 网络库。
- `/Fe:rudp.exe`：输出可执行文件名为 `rudp.exe`。

## 二、运行方式

### 1. 启动接收端

在终端运行：

```bat
rudp.exe recv <port> <output_file> [window_size]
```

示例：

```bat
rudp.exe recv 9000 recv_output.bin 128
```

含义：
- `<port>`：接收端监听的 UDP 端口号，例如 `9000`。
- `<output_file>`：接收到的数据写入的输出文件名，例如 `recv_output.bin`。
- `[window_size]`（可选）：接收端允许的滑动窗口大小（分组数，默认 64），越大可同时缓存的乱序分组越多，发送端能保持更高吞吐。

### 2. 启动发送端

在另一个终端运行：

```bat
rudp.exe send <server_ip> <port> <input_file> [delay_ms] [loss_percent]
```

示例 1：不模拟延迟和丢包

```bat
rudp.exe send 127.0.0.1 9000 input.bin
```

示例 2：模拟 10ms 单向链路延迟、5% 丢包率

```bat
rudp.exe send 127.0.0.1 9000 input.bin 10 5
```

参数含义：
- `<server_ip>`：接收端的 IP 地址，例如 `127.0.0.1`。
- `<port>`：接收端监听的端口号，需与接收端一致，例如 `9000`。
- `<input_file>`：待发送的输入文件名，例如 `input.bin`。
- `[delay_ms]`（可选）：模拟链路**单向延迟**，单位毫秒，例如 `10` 表示 10ms。
- `[loss_percent]`（可选）：模拟**丢包率百分比**，例如 `5` 表示 5%（每个数据包独立以 0.05 概率被丢弃）。

当提供可选参数时，程序会调用 `setLinkOptions(delay_ms, loss_percent/100)`，在发送端内部通过 `g_linkDelayMs` 和 `g_lossRate` 模拟链路延迟和随机丢包。

## 三、各源文件作用说明

- main.cpp：程序入口，解析命令行参数，调用发送端/接收端，并设置延迟和丢包率。
- rudp.h：公共头文件，定义协议常量、报文头结构、SACK 结构及函数/全局变量声明。
- rudp_common.cpp：公共工具函数，实现校验和、发送/接收封装、超时设置和链路参数设置。
- rudp_sender.cpp：发送端实现，负责三次握手、文件分块发送、滑动窗口与重传、四次挥手和统计输出。
- rudp_receiver.cpp：接收端实现，负责三次握手、乱序缓存和按序写文件、发送 ACK+SACK 以及被动四次挥手。


