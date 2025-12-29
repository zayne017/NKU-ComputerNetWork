#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>
#include <windows.h>

// 实验配置参数
const int MSS = 8192;           // 数据载荷大小
const int RCV_WND_SIZE = 50;    // 接收窗口大小固定为 50 (流量控制) 缓冲区只能存50个包
const double TIMEOUT_SEC = 0.5; // 超时重传时间 (秒)

// 标志位 
const uint8_t FLAG_DATA = 0x00;
const uint8_t FLAG_SYN = 0x01; // 建立连接
const uint8_t FLAG_ACK = 0x02; // 确认
const uint8_t FLAG_FIN = 0x04; // 关闭连接

// 协议头结构 
#pragma pack(push, 1) // 强制1字节对齐 防止后面填充废数据
struct MyHeader {
    uint32_t seq;       // 序列号
    uint32_t ack;       // 确认的ack 期望收到的下一个序号
    uint32_t sack_seq;  // SACK：收到的那个乱序包的序号
    uint16_t length;    // 数据长度
    uint16_t checksum;  // 校验和
    uint8_t  flags;     // 标志位
    uint16_t rwnd;      // 接收窗口通告

    MyHeader() : seq(0), ack(0), sack_seq(0), length(0), checksum(0), flags(0), rwnd(RCV_WND_SIZE) {}
};
#pragma pack(pop)

// 数据包
struct Packet {
    MyHeader header;
    char data[MSS];
};

// 校验和计算函数 
inline uint16_t calculate_checksum(Packet* pkt)
{
    // 先把原来存的校验和拿出来备份
    uint16_t old_sum = pkt->header.checksum;
    pkt->header.checksum = 0; // 计算前先清零
    unsigned long sum = 0;
    uint16_t* ptr = (uint16_t*)pkt;
    // 计算所有数据的长度（头 + 实际数据长度）
    int size = sizeof(MyHeader) + pkt->header.length;
    // 两两字节拼接成16位 整数相加
    while (size > 1) {
        sum += *ptr++;
        size -= 2;
    }
    // 如果只有奇数个字节，处理剩下的这一个
    if (size) sum += *(uint8_t*)ptr;
    // 折叠进位 低16位和高16位
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    pkt->header.checksum = old_sum; //恢复原来的校验和值
    return (uint16_t)(~sum);//取反返回
}

#endif