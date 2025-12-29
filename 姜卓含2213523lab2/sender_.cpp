#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <fstream>
#include <vector>
#include <ctime>
#include <algorithm>
#include <iomanip>
#include "protocol.h"

#pragma comment(lib, "ws2_32.lib")

using namespace std;

// Reno状态定义 慢启动 拥塞避免 快重传
enum RenoState { SLOW_START, CONGESTION_AVOIDANCE, FAST_RECOVERY };

// 已发送包的记录 (用于重传和 SACK)
struct SentPacket
{
    Packet pkt;         // 包本身的内容
    bool is_acked;      // 选择重传SACK标记：若为true，则超时也不重传
    clock_t send_time;  // 发送时间用来计算是否超时
};

// 全局变量 用于Reno
double cwnd = 1.0;            // 拥塞窗口
double ssthresh = 16.0;       // 慢启动阈值
int dup_ack_count = 0;        // 重复ACK计数 用来触发快重传
RenoState state = SLOW_START; // 当前状态

SOCKET sendSocket;
sockaddr_in routerAddr;// 地址的结构体
int addrLen = sizeof(routerAddr);

// 颜色控制函数
// 10=绿色(正常), 12=红色(丢包/重传), 14=黄色(状态变化), 7=白色(默认)
void SetColor(int colorID)
{
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), colorID);
}
// 发送函数 发给Router
void send_raw(Packet& pkt)
{
    pkt.header.checksum = calculate_checksum(&pkt);// 计算校验和
    sendto(sendSocket, (char*)&pkt, sizeof(MyHeader) + pkt.header.length, 0, (sockaddr*)&routerAddr, addrLen);
}

// 打印状态 
void log_status(const char* event, uint32_t base)
{
    // 根据事件类型变色
    if (strcmp(event, "Timeout") == 0 || strcmp(event, "Fast Retransmit") == 0) {
        SetColor(12); // 红色：出问题了
    }
    else if (strcmp(event, "New ACK") == 0) {
        SetColor(10); // 绿色：正常推进
    }
    else {
        SetColor(14); // 黄色：其他状态
    }

    cout << "[" << event << "] base=" << base
        << " cwnd=" << fixed << setprecision(1) << cwnd
        << " ssthresh=" << (int)ssthresh
        << " state=" << (state == SLOW_START ? "SLOW" : (state == CONGESTION_AVOIDANCE ? "AVOID" : "RECOV"))
        << endl;

    SetColor(7); // 打印完恢复白色
}

int main()
{
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);// 初始化 Windows Socket 库，版本2.2
    // 创建一个 UDP 套接字
    sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    // 设置非阻塞模式
    u_long mode = 1;
    ioctlsocket(sendSocket, FIONBIO, &mode);

    // 目标地址: Router 端口8000
    routerAddr.sin_family = AF_INET;
    routerAddr.sin_port = htons(8000);
    routerAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    cout << "请输入文件名: ";
    string filename;
    cin >> filename;
    // 以二进制模式打开文件 ios::binary
    ifstream inFile(filename, ios::binary);
    if (!inFile) { cout << "文件不存在！" << endl; return 0; }
    // 把整个文件一次性读进vector<char>数组里
    vector<char> fileBuffer((istreambuf_iterator<char>(inFile)), istreambuf_iterator<char>());
    inFile.close();
    int total_bytes = fileBuffer.size();// 获取文件总大小

    // 1. 建立连接 (三次握手)
    cout << "--- 开始握手 ---" << endl;
    uint32_t base_seq = 0;// 初始序列号 从0开始
    Packet syn_pkt; // 造一个 SYN 包 第一次握手
    syn_pkt.header.flags = FLAG_SYN;// 请求连接 
    syn_pkt.header.seq = base_seq;

    bool handshaked = false;
    clock_t timer = clock();// 开始计时
    while (!handshaked)// 循环直到建立连接
    {
        // 超时重传SYN 检查现在时间-上次发送时间>0.5秒
        if ((double)(clock() - timer) / CLOCKS_PER_SEC > TIMEOUT_SEC)
        {
            send_raw(syn_pkt); //重发 SYN
            timer = clock(); // 重新计时
            SetColor(12); // 红色
            cout << "超时，重发 SYN..." << endl;
            SetColor(7);
        }

        Packet recv_pkt;
        if (recvfrom(sendSocket, (char*)&recv_pkt, sizeof(Packet), 0, NULL, NULL) > 0)
        {
            // 收到 SYN + ACK 接收第二次握手
            if (recv_pkt.header.flags & (FLAG_SYN | FLAG_ACK))
            {
                handshaked = true;
                base_seq++;// 要消耗一个序列号
                SetColor(10);
                cout << "收到 SYN+ACK，连接建立！" << endl;
                SetColor(7);

                // 发送第三次握手 ACK
                Packet third_ack;
                third_ack.header.seq = base_seq;             // 我的序列号 1
                third_ack.header.ack = recv_pkt.header.seq + 1; // 确认对方的序列号 seq+1
                third_ack.header.flags = FLAG_ACK;           // 纯 ACK 标记
                third_ack.header.length = 0;                 // 不带数据
                third_ack.header.sack_seq = 0;

                send_raw(third_ack); // 发送给 Router
                cout << "已发送第三次握手 ACK。" << endl;
            }
        }
    }

    // 2. 数据传输主循环
    cout << "--- 开始传输 (SACK + Reno) ---" << endl;
    uint32_t send_base = base_seq;// 发送窗口左边缘 还没确认的最小序号
    uint32_t next_seq = base_seq;// 发送窗口右边缘
    vector<SentPacket> window; // 发送窗口缓冲区
    clock_t start_time = clock();// 计时
    // 循环 当还没确认完或窗口非空时
    while (send_base < base_seq + total_bytes || !window.empty())
    {
        // A. 发送新数据 
        // 实际发送窗口 = min(拥塞窗口cwnd, 接收端通告窗口rwnd)
        int win_size = min((int)cwnd, RCV_WND_SIZE);
        // 窗口还没满且文件还没发完时 进行发送
        while (window.size() < win_size && next_seq < base_seq + total_bytes)
        {
            Packet pkt;
            pkt.header.seq = next_seq;// 序号
            int pos = next_seq - base_seq;// 当前位置
            int len = min(MSS, total_bytes - pos);// MSS和当前剩下的数据的最小值
            memcpy(pkt.data, &fileBuffer[pos], len);//拷贝数据到包里
            pkt.header.length = len;

            send_raw(pkt);//发送
            window.push_back({ pkt, false, clock() });//存档到缓冲区 
            next_seq += len;// 处理下一段
        }
        // B. 接收 ACK 
        Packet recv_pkt;
        if (recvfrom(sendSocket, (char*)&recv_pkt, sizeof(Packet), 0, NULL, NULL) > 0)
        {
            if (calculate_checksum(&recv_pkt) != recv_pkt.header.checksum)
            {
                SetColor(12); // 变红
                cout << "[Error] Checksum Failed! Drop corrupted packet." << endl;
                SetColor(7);  // 恢复
                continue;     // 直接跳过，不处理这个坏包
            }
            uint32_t ack = recv_pkt.header.ack;// 累积确认：对方期望的下一个序号
            uint32_t sack = recv_pkt.header.sack_seq;// 选择确认：对方收到的乱序包

            // SACK 处理：标记被 SACK 的包
            if (sack != 0)
            {
                for (auto& p : window)// 遍历已发送窗口
                    if (p.pkt.header.seq == sack)// 找到 标记已送达
                        p.is_acked = true;
            }
            if (ack > send_base)
            { // 收到新 ACK 要更新cwnd
                send_base = ack;// 更新send_base
                // 滑动窗口：移除已确认的 把序号小于新send_base的包，从内存里删掉
                while (!window.empty() && window.front().pkt.header.seq < send_base)
                    window.erase(window.begin());

                // Reno 状态迁移
                dup_ack_count = 0;// 重置重复ACK计数器
                if (state == SLOW_START)// 如果是慢启动 指数增长 每过一个RTT翻倍
                {
                    cwnd += 1.0;
                    if (cwnd >= ssthresh)
                        state = CONGESTION_AVOIDANCE;
                }
                else if (state == CONGESTION_AVOIDANCE)// 拥塞避免 线性
                {
                    cwnd += 1.0 / (int)cwnd;// 每个ACK，窗口增加1/cwnd，一个RTT cwnd个ACK 总共增加1 
                }
                else
                { // 快速恢复 -> 拥塞避免
                    state = CONGESTION_AVOIDANCE;
                    cwnd = ssthresh;
                }
                log_status("New ACK", send_base);// 打印日志
            }
            else
            {
                dup_ack_count++;// 收到重复 ACK 计数器加一
                if (dup_ack_count == 3) // 累计3次
                {
                    // Reno: 快速重传
                    log_status("Fast Retransmit", send_base);
                    state = FAST_RECOVERY;// 快恢复状态
                    ssthresh = max(2.0, cwnd / 2.0);//阈值减半 最小是2防止窗口太小
                    cwnd = ssthresh + 3;
                    if (!window.empty())
                    { // 重传窗口左边最老的那个包
                        window.front().send_time = clock();// 重置计时器
                        send_raw(window.front().pkt);// 重发那个丢的包
                    }
                }
                else if (state == FAST_RECOVERY) cwnd += 1.0;// 进入拥塞避免状态
            }

        }
        // C. 超时重传 
        if (!window.empty())
        {
            SentPacket& base = window.front();//窗口最左边的包
            // SACK：如果 Base 包已经被 SACK，就不重传
            // 只有没有确认标记且超时的才重传
            if (!base.is_acked && (double)(clock() - base.send_time) / CLOCKS_PER_SEC > TIMEOUT_SEC)
            {
                log_status("Timeout", base.pkt.header.seq);
                // Reno: 超时惩罚
                ssthresh = max(2.0, cwnd / 2.0);// 阈值减半
                cwnd = 1.0;// 从1.0开始慢启动
                state = SLOW_START;
                dup_ack_count = 0;
                base.send_time = clock();
                send_raw(base.pkt);// 重发
            }
        }
    }

    // 3.断开连接
    SetColor(10);
    cout << "传输完成！" << endl;
    SetColor(7);

    // 显示时间和吞吐率
    double duration = (double)(clock() - start_time) / CLOCKS_PER_SEC;
    if (duration == 0)
        duration = 0.001; // 防止除以0

    cout << "========================================" << endl;
    cout << "传输时间: " << duration << " s" << endl;
    cout << "文件大小: " << total_bytes / 1024.0 << " KB" << endl;
    cout << "平均吞吐率: " << (total_bytes / 1024.0) / duration << " KB/s" << endl;
    cout << "========================================" << endl;

    cout << "正在关闭连接..." << endl;

    // 4.关闭连接 四次挥手
    // 阶段 1: 发送 FIN (第一次)，等待 ACK (第二次)
    Packet fin;// 准备FIN包
    fin.header.flags = FLAG_FIN;// 标志位
    fin.header.seq = next_seq;
    fin.header.length = 0;
    fin.header.checksum = 0;

    bool fin_acked_by_receiver = false;//标记
    int retry = 0;

    while (!fin_acked_by_receiver && retry < 5)
    {
        send_raw(fin); // 发送 FIN
        cout << "[FIN] 发送断开请求 (第一次挥手) - 尝试 " << retry + 1 << endl;
        // 启动计时器，等 0.5 秒
        clock_t timer = clock();
        while ((double)(clock() - timer) / CLOCKS_PER_SEC < 0.5)
        {
            Packet response;
            if (recvfrom(sendSocket, (char*)&response, sizeof(Packet), 0, NULL, NULL) > 0) 
            {
                if (calculate_checksum(&response) == response.header.checksum)
                {
                    // 第二次挥手，如果收到 ACK
                    if (response.header.flags & FLAG_ACK)
                    {
                        fin_acked_by_receiver = true;
                        cout << "[Recv ACK] 收到服务端确认 (第二次挥手)" << endl;                                                
                        if (response.header.flags & FLAG_FIN)
                        {
                            cout << "[Recv FIN] 同时也收到了对方的 FIN (第三次挥手)" << endl;
                            goto SEND_FINAL_ACK; // 直接跳到最后
                        }
                        break;
                    }
                }
            }
        }
        retry++;// 没收到重复发FIN
    }
    if (!fin_acked_by_receiver) 
    {
        cout << "[Warn] 对方未响应 FIN，强制关闭。" << endl;
        goto END_CONNECTION;
    }
    // 阶段 2: 等待对方的 FIN (第三次挥手)
    {
        cout << "[Wait] 等待对方发送 FIN..." << endl;
        clock_t wait_fin_timer = clock();
        bool received_server_fin = false;

        // 最多等 2 秒
        while ((double)(clock() - wait_fin_timer) / CLOCKS_PER_SEC < 2.0) 
        {
            Packet response;
            if (recvfrom(sendSocket, (char*)&response, sizeof(Packet), 0, NULL, NULL) > 0)
            {
                if (calculate_checksum(&response) == response.header.checksum)
                {
                    if (response.header.flags & FLAG_FIN)
                    {
                        cout << "[Recv FIN] 收到对方断开请求 (第三次挥手)" << endl;
                        received_server_fin = true;
                        goto SEND_FINAL_ACK;
                    }
                }
            }
        }
        cout << "[Warn] 等待对方 FIN 超时。" << endl;
        goto END_CONNECTION;
    }
    // 阶段 3: 发送最终 ACK (第四次挥手)
SEND_FINAL_ACK:
    {
        Packet final_ack;
        final_ack.header.flags = FLAG_ACK;
        final_ack.header.seq = next_seq + 1; 
        final_ack.header.length = 0;
        final_ack.header.ack = 0;

        // 重复发 3 次确保对方收到
        for (int i = 0; i < 3; i++)
        {
            send_raw(final_ack);
            Sleep(20);
        }
        SetColor(10);
        cout << "[Send ACK] 发送最终确认 (第四次挥手) - 挥手结束" << endl;
        SetColor(7);
    }

END_CONNECTION:
    closesocket(sendSocket);// 归还网卡资源
    WSACleanup();// 卸载库
    system("pause");
    return 0;
}