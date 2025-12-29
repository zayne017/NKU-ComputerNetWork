#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <fstream>
#include <map>
#include <string>
#include "protocol.h"

#pragma comment(lib, "ws2_32.lib")

using namespace std;

// 流水线：乱序缓冲区 SACK
// Key: 序列号, Value: 数据内容
// 收到乱序包不丢弃，先存起来，实现选择确认SACK
map<uint32_t, string> packet_buffer;
uint32_t expected_seq = 0; // 期望收到的序列号 滑动窗口最左边

// 颜色控制函数
// 10=绿色(正常), 12=红色(错误/重复), 14=黄色(状态/乱序), 7=白色(默认)
void SetColor(int colorID) 
{
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), colorID);
}

// 发送 ACK 函数
void send_ack(SOCKET sock, sockaddr_in& target, uint32_t ack_val, uint32_t sack_val, uint8_t flags) 
{
    Packet pkt;
    pkt.header.seq = 0;
    pkt.header.ack = ack_val;       // 累积确认
    pkt.header.sack_seq = sack_val; // SACK：反馈乱序包
    pkt.header.flags = flags | FLAG_ACK;
    pkt.header.rwnd = RCV_WND_SIZE; // 流量控制：通告固定窗口
    pkt.header.length = 0;
    pkt.header.checksum = calculate_checksum(&pkt);

    sendto(sock, (char*)&pkt, sizeof(MyHeader), 0, (sockaddr*)&target, sizeof(target));
}

int main() 
{
    // 初始化 Windows Socket DLL，版本请求 2.2
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    // 数据报套接字 UDP
    SOCKET recvSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    sockaddr_in recvAddr, senderAddr;// 两个地址
    int senderLen = sizeof(senderAddr);
    recvAddr.sin_family = AF_INET;// 使用 IPv4
    recvAddr.sin_port = htons(8001);// 端口号 8001
    recvAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // receiver 服务器需要绑定 把recvSocket和recvAddr绑定在一起
    if (bind(recvSocket, (sockaddr*)&recvAddr, sizeof(recvAddr)) == SOCKET_ERROR)
    {
        cout << "Bind Error! Port 8001 busy." << endl; return 1;
    }
    cout << "Receiver Ready on Port 8001..." << endl;

    ofstream outFile;
    bool connected = false;// 连接状态

    // 统计变量
    clock_t start_time = 0;
    long total_bytes_received = 0;// 记录一共收了多少字节

    while (true) 
    {
        Packet pkt;
        int len = recvfrom(recvSocket, (char*)&pkt, sizeof(Packet), 0, (sockaddr*)&senderAddr, &senderLen);
        if (len <= 0) continue;

        // 差错检测
        if (calculate_checksum(&pkt) != pkt.header.checksum) {
            SetColor(12); // 红色
            cout << "[Checksum Error] Drop seq=" << pkt.header.seq << endl;
            SetColor(7);
            continue;// 跳过，丢弃这个包
        }

        // 1.建立连接 (三次握手)
        // 收到 SYN (第一次握手)
        if (pkt.header.flags & FLAG_SYN) 
        {
            SetColor(14); // 黄色
            cout << "[Recv SYN] New Connection Request." << endl;
            SetColor(7);

            expected_seq = pkt.header.seq + 1;// 序列号+1

            // 回复 SYN + ACK (第二次握手)
            send_ack(recvSocket, senderAddr, expected_seq, 0, FLAG_SYN);

            // 设为 false 在等第三次握手
            connected = false;

            // 预先准备好文件和计数器
            if (outFile.is_open()) outFile.close();
            outFile.open("received_file.jpg", ios::binary);
            packet_buffer.clear();// 清空缓冲区
            start_time = clock();
            total_bytes_received = 0;
            continue;
        }

        // 处理第三次握手 (ACK)
        // 如果当前状态是未连接 false，但收到了一个纯 ACK 包，说明这是 Sender 的回应
        if (!connected && (pkt.header.flags & FLAG_ACK) && !(pkt.header.flags & FLAG_SYN) && pkt.header.length == 0)
        {
            connected = true; // 正式建立连接
            SetColor(10); // 绿色
            cout << "[Handshake] Recv 3rd ACK. Connection Established!" << endl;
            SetColor(7);
            continue;
        }

        // 如果第三次握手的 ACK 丢了，Sender 直接发来了数据包
        if (!connected && pkt.header.length > 0) 
        {
            SetColor(14);
            cout << "[Handshake] Implicit Connection (Data received)." << endl;
            SetColor(7);
            connected = true; // 隐式确认连接成功
        }

        // 2.关闭连接：处理 FIN (四次挥手)
        // 收到了 Sender 发来的包，且包头里有 FIN 标记
        if (pkt.header.flags & FLAG_FIN) 
        {
            SetColor(14); // 黄色
            cout << endl << "[Recv FIN] 收到断开请求 (第一次挥手)" << endl;
            SetColor(7);

            expected_seq = pkt.header.seq + 1;//序列号更新

            // 发送 ACK (第二次挥手)
            // 告诉 Sender
            send_ack(recvSocket, senderAddr, expected_seq, 0, 0);
            cout << "[Send ACK] 发送确认 (第二次挥手)" << endl;

            // 发送 FIN (第三次挥手) 并等待最终 ACK
            // 构造 FIN 包
            Packet fin_pkt;
            fin_pkt.header.seq = 0; 
            fin_pkt.header.ack = pkt.header.seq + 1;
            fin_pkt.header.sack_seq = 0;
            fin_pkt.header.flags = FLAG_FIN | FLAG_ACK; // FIN + ACK
            fin_pkt.header.rwnd = RCV_WND_SIZE;
            fin_pkt.header.length = 0;
            fin_pkt.header.checksum = calculate_checksum(&fin_pkt);

            bool final_ack_received = false;// 标记：是否收到最后的 ACK
            int retries = 0;

            // 循环：只要没收到最终 ACK，且循环不到 5 次，就一直循环
            while (!final_ack_received && retries < 5) 
            {
                // 发送 FIN 第三次挥手
                sendto(recvSocket, (char*)&fin_pkt, sizeof(MyHeader), 0, (sockaddr*)&senderAddr, sizeof(senderAddr));
                cout << "[Send FIN] 发送结束请求 (第三次挥手) - 尝试 " << retries + 1 << endl;

                // 计时等待 ACK
                clock_t wait_timer = clock();
                while ((double)(clock() - wait_timer) / CLOCKS_PER_SEC < 0.5)
                {
                    Packet response;
                    int len = recvfrom(recvSocket, (char*)&response, sizeof(Packet), 0, (sockaddr*)&senderAddr, &senderLen);
                    if (len > 0) {
                        if (calculate_checksum(&response) == response.header.checksum) {
                            // 收到 只有ACK的包 (第四次挥手)
                            if ((response.header.flags & FLAG_ACK) && !(response.header.flags & FLAG_FIN)) {
                                final_ack_received = true;
                                SetColor(10); // 绿色
                                cout << "[Recv ACK] 收到最终确认 (第四次挥手) - 连接关闭" << endl;
                                SetColor(7);
                                break;
                            }
                        }
                    }
                }
                retries++;// 如果 0.5s 到了还没 break，说明超时，重试次数+1
            }

            if (!final_ack_received)// 强制关闭
            {
                SetColor(12);
                cout << "[Warn] 等待最终 ACK 超时，强制关闭。" << endl;
                SetColor(7);
            }

            // 统计与重置
            if (outFile.is_open()) 
                outFile.close();
            connected = false;
            packet_buffer.clear();

            // 显示统计
            double duration = (double)(clock() - start_time) / CLOCKS_PER_SEC;
            if (duration == 0) duration = 0.001;
            SetColor(10);
            cout << "========================================" << endl;
            cout << "传输成功!" << endl;
            cout << "时间: " << duration << " s" << endl;
            cout << "吞吐率: " << (total_bytes_received / 1024.0) / duration << " KB/s" << endl;
            cout << "========================================" << endl;
            SetColor(7);
            break;
        }

        // 3.流水线数据处理 SACK
        if (connected && pkt.header.length > 0)
        {
            // A. 顺序到达 (Seq == Expected)
            if (pkt.header.seq == expected_seq) 
            {
                // 直接写入文件
                if (outFile.is_open()) outFile.write(pkt.data, pkt.header.length);
                expected_seq += pkt.header.length;// 窗口右移 更新序号

                // 统计字节
                total_bytes_received += pkt.header.length;

                // 正常接收日志 
                SetColor(10); 
                cout << "[Data] Recv Ordered Seq=" << pkt.header.seq << endl;
                SetColor(7);

                // 检查缓冲区：有没有能接上的包 累积确认
                while (packet_buffer.count(expected_seq)) 
                {
                    string& data = packet_buffer[expected_seq];

                    // 黄色显示缓冲取出
                    SetColor(14);
                    cout << "[Buffer] Pop Buffered Seq=" << expected_seq << " (Cache Hit)" << endl;
                    SetColor(7);
                    // 写入文件
                    if (outFile.is_open()) outFile.write(data.c_str(), data.length());
                    // 右移窗口 并从缓存删除
                    uint32_t len = data.length();
                    packet_buffer.erase(expected_seq);
                    expected_seq += len;

                    // 统计缓冲区的字节
                    total_bytes_received += len;
                }
                // 发送累积确认
                send_ack(recvSocket, senderAddr, expected_seq, 0, 0);
            }
            // B. 乱序到达 (Seq > Expected) -> 存入 Buffer
            else if (pkt.header.seq > expected_seq)
            {
                // 黄色显示乱序
                SetColor(14);
                cout << "[SACK] Recv Out-of-Order Seq=" << pkt.header.seq << " (Buffered)" << endl;
                SetColor(7);
                // 存入缓冲区 map可以自动按seq排序
                string data(pkt.data, pkt.header.length);
                packet_buffer[pkt.header.seq] = data;

                // 发送 SACK：ack=期望的, sack=刚收到的
                send_ack(recvSocket, senderAddr, expected_seq, pkt.header.seq, 0);
            }
            // C. 重复包 -> 重发 ACK
            else
            {
                // 红色显示重复包
                SetColor(12);
                cout << "[Dup] Recv Duplicate Seq=" << pkt.header.seq << " (Ignored)" << endl;
                SetColor(7);

                send_ack(recvSocket, senderAddr, expected_seq, 0, 0);
            }
        }
    }
    cout << endl;
    cout << "Receiver 已停止工作，请按任意键退出..." << endl;
    system("pause");
    // 正式关闭
    closesocket(recvSocket); 
    WSACleanup();            // 卸载库

    return 0;
}