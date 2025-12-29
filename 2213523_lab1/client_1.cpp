#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <processthreadsapi.h>
#include <windows.h>
#include <string>      
#include <sstream>   
#pragma comment(lib, "ws2_32.lib")
using namespace std;

DWORD WINAPI RecvThread(LPVOID lpParameter)//客户端的接收消息线程 
{
    SOCKET clientSocket = (SOCKET)lpParameter;//强制转换为SOCKET类型
    char recvBuf[1024];//缓冲区 保存消息
    int recvResult;//收到消息字节数

    while (true)
    {
        recvResult = recv(clientSocket, recvBuf, sizeof(recvBuf), 0);//recv阻塞函数
        if (recvResult > 0)//成功收到消息
        {
            recvBuf[recvResult] = '\0';//加一个\0表示字符串结束
            std::string command(recvBuf);//拷贝到string字符串对象
            //解析服务器发来的协议
            if (command.find("PUB:") == 0)//如果command是PUB开头 是公聊 
            {
                cout << "  (公聊) " << command.substr(4) << endl;//从第4个索引位置开始输出
            }
            else if (command.find("PRIV:") == 0)
            {
                cout << "  (私聊) " << command.substr(5) << endl;
            }
            else if (command.find("INFO:") == 0)//系统通知
            {
                cout << "  [系统] " << command.substr(5) << endl;
            }
            else if (command.find("LIST:") == 0)//显示用户列表
            {
                cout << "  [在线用户]: " << command.substr(5) << endl;
            }
            else
            {
                cout << "  (收到未知): " << command << endl;
            }
        }
        else if (recvResult == 0)
        {
            cout << ">>> 服务器已关闭连接。" << endl;
            break;
        }
        else
        {
            cout << ">>> 接收失败，连接中断: " << WSAGetLastError() << endl;
            break;
        }
    }
    return 0;
}

int main()
{
    SetConsoleOutputCP(936);//设置控制台输出为GBK 
    SetConsoleCP(936);//设置控制台输入为GBK
    cout << "--- 客户端启动 ---" << endl;

    WSADATA wsadata;
    WSAStartup(MAKEWORD(2, 2), &wsadata);
    cout << "Winsock初始化成功" << endl;

    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, 0);//IPv4 TCP流式套接字
    cout << "Socket创建成功" << endl;

    SOCKADDR_IN serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(12345);//转换为网络字节序
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr.S_un.S_addr);
    cout << "准备连接服务器(127.0.0.1:12345)" << endl;

    if (connect(clientSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        cout << "连接失败: " << WSAGetLastError() << endl;
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }
    cout << "连接服务器成功，请开始聊天" << endl;

    std::string username;
    cout << "请输入您的用户名: ";
    getline(cin, username);

    std::string joinMsg = "JOIN:" + username;
    send(clientSocket, joinMsg.c_str(), joinMsg.length() + 1, 0);

    CreateThread(NULL, 0, RecvThread, (LPVOID)clientSocket, 0, NULL);//接收消息的线程

    cout << "--- 已进入聊天室 ---" << endl;
    cout << "  @用户名[空格]消息 (私聊)" << endl;
    cout << "  list              (看列表)" << endl;
    cout << "  quit              (退出)" << endl;
    cout << "  其他              (公聊)" << endl;
     
    std::string inputLine;//用户输入
    while (true) 
    {
        getline(cin, inputLine);//阻塞在这里等待用户输入
        if (inputLine.empty()) continue;
        std::string sendBuf; //发给服务器的协议字符串

        if (inputLine == "quit")//主动退出 
        {
            sendBuf = "QUIT:";
            send(clientSocket, sendBuf.c_str(), sendBuf.length() + 1, 0);
            cout << ">>> 你已断开连接" << endl;
            break; 
        }
        else if (inputLine == "list")//用户列表 
        {
            sendBuf = "LIST:";
        }
        else if (inputLine[0] == '@')//检测是否私聊
        {
            size_t separatorPos = inputLine.find(' ');//查找第一个空格
            if (separatorPos != std::string::npos)//如果找到
            {
                std::string targetUser = inputLine.substr(1, separatorPos - 1);//私聊对象 
                std::string msg = inputLine.substr(separatorPos + 1);//私聊消息   
                sendBuf = "PRIV:" + targetUser + ":" + msg;//私聊协议
            }
            else 
            {
                cout << "  [系统] 私聊格式错误, 应为: @用户名[空格]消息" << endl;
                continue; //本次输入无效 不发送
            }
        }
        else//默认为公聊
        {
            sendBuf = "MSG:" + inputLine;
        }
        //统一发送
        int sendResult = send(clientSocket, sendBuf.c_str(), sendBuf.length() + 1, 0);
        if (sendResult == SOCKET_ERROR) 
        {
            cout << ">>> 发送失败: " << WSAGetLastError() << endl;
            break;
        }
    }
    closesocket(clientSocket);
    WSACleanup();
    system("pause");
    return 0;
}