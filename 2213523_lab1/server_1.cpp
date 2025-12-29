#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#include <string>       
#include <sstream>     
#include <map>  
#include <ctime>//用于获取时间
#include <iomanip>//用于格式化时间 (setw, setfill)
#include <cstring>
#include <chrono>//用于获取高精度时间
#pragma comment(lib, "ws2_32.lib")
using namespace std;

//全局变量
std::map<SOCKET, std::string> g_clients;//采用map存储所有连接的客户端
CRITICAL_SECTION g_cs; //锁 保护临界区

std::string getCurrentTimestamp()//获取当前时间 
{
    try 
    {
        auto now = std::chrono::system_clock::now();//获取当前系统时间点
        std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);//转换为time_t
        std::tm tm_info;//转换为tm结构体 本地时间
        if (localtime_s(&tm_info, &now_time_t) != 0)
        {
            return "[TimeError] ";
        }
        std::stringstream ss;//使用 stringstream 格式化
        ss << "[" << std::setfill('0') << std::setw(2) << tm_info.tm_hour << ":"
            << std::setfill('0') << std::setw(2) << tm_info.tm_min << ":"
            << std::setfill('0') << std::setw(2) << tm_info.tm_sec << "] ";
        return ss.str();
    }
    catch (...) 
    {
        return "[TimeError] ";
    }
}
//广播函数
void BroadcastMessage(const std::string& message, SOCKET skipSocket) 
{
    EnterCriticalSection(&g_cs);//加锁
    for (auto& pair : g_clients)//遍历 pair.first是SOCKET pair.second是用户名 
    {
        if (pair.first != skipSocket)//不发给发送者自身
        { 
            send(pair.first, message.c_str(), message.length() + 1, 0);
        }
    }
    LeaveCriticalSection(&g_cs); //解锁
}

//私聊函数
void SendPrivateMessage(const std::string& message, const std::string& targetUser, const std::string& fromUser) 
{
    EnterCriticalSection(&g_cs); //加锁

    SOCKET targetSocket = INVALID_SOCKET;
    for (auto& pair : g_clients)
    {
        if (pair.second == targetUser)
        { 
            targetSocket = pair.first;
            break;
        }
    }
    if (targetSocket != INVALID_SOCKET)//如果找到则发送
    {
        send(targetSocket, message.c_str(), message.length() + 1, 0);
    }
    else 
    {
        cout << "  (私聊失败: " << fromUser << " 找不到 " << targetUser << ")" << endl;
        SOCKET fromSocket = INVALID_SOCKET;
        for (auto& pair : g_clients)//找发送者 
        {
            if (pair.second == fromUser) 
            { 
                fromSocket = pair.first; 
                break;
            }
        }
        if (fromSocket != INVALID_SOCKET)
        {
            std::string failMsg = "INFO:私聊失败, 找不到用户: " + targetUser;
            send(fromSocket, failMsg.c_str(), failMsg.length() + 1, 0);//把"找不到用户"的消息发回给发送者
        }
    }
    LeaveCriticalSection(&g_cs); //解锁
}

//获取用户列表函数
void SendUserList(SOCKET clientSocket)
{
    EnterCriticalSection(&g_cs); //加锁
    std::string userList = "LIST:";
    for (auto& pair : g_clients)//遍历map，把所有用户名(pair.second)拼起来
    {
        userList += pair.second + ",";
    }
    if (!g_clients.empty())
    {
        userList.pop_back(); //去掉最后一个多余的','
    }
    LeaveCriticalSection(&g_cs); //解锁
    send(clientSocket, userList.c_str(), userList.length() + 1, 0);//发送给请求者
}

DWORD WINAPI ClientThread(LPVOID lpParameter)
{
    SOCKET clientSocket = (SOCKET)lpParameter;
    std::string username; //线程对应的用户名
    char recvBuf[1024];//客户端发送的第一条消息
    int recvResult = recv(clientSocket, recvBuf, sizeof(recvBuf), 0);
    if (recvResult <= 0) 
    {
        cout << ">>> 客户端" << clientSocket << "断开。" << endl;
        closesocket(clientSocket);
        return 1;
    }
    recvBuf[recvResult] = '\0';
    if (strncmp(recvBuf, "JOIN:", 5) == 0)//解析"JOIN:用户名"
    {
        username = recvBuf + 5; 
    }
    else 
    {
        cout << ">>> 客户端 " << clientSocket << " 协议错误，未发送JOIN。" << endl;
        send(clientSocket, "INFO:协议错误,请先JOIN\0", 25, 0);
        closesocket(clientSocket);
        return 1;
    }
    bool isDuplicate = false; //用户名查重 假设不是重复的
    EnterCriticalSection(&g_cs);
    for (auto& pair : g_clients) 
    {
        if (pair.second == username) 
        {
            isDuplicate = true; //发现用户名重复
            break;
        }
    }
    if (isDuplicate) //如果重复
    {
        LeaveCriticalSection(&g_cs); 
        cout << ">>> " << username << " (Socket: " << clientSocket << ") 尝试加入, 但用户名已存在。" << endl;
        send(clientSocket, "INFO:用户名已被占用, 请重试\0", 31, 0);
        closesocket(clientSocket);
        return 1; //线程结束
    }
    else 
    {
        g_clients[clientSocket] = username; //将(Socket, Username)存入 map
        LeaveCriticalSection(&g_cs); //解锁
    }
    cout << ">>> " << username << " (Socket: " << clientSocket << ") 加入了聊天室。" << endl;
    std::string joinMsg = "INFO:" + username + " 加入了聊天室。";
    BroadcastMessage(joinMsg, clientSocket); //广播给别人"xxx 加入"的消息
    send(clientSocket, "INFO:欢迎加入！\0", 18, 0); //单独欢迎自己

    do//聊天部分 
    {
        memset(recvBuf, 0, sizeof(recvBuf)); //清空缓冲区
        recvResult = recv(clientSocket, recvBuf, sizeof(recvBuf), 0);
        if (recvResult > 0) 
        {
            recvBuf[recvResult] = '\0'; 
            std::string command(recvBuf);
            //解析公聊MSG
            if (command.find("MSG:") == 0)
            {
                std::string msg = command.substr(4);
                std::string timestamp = getCurrentTimestamp(); //获取时间戳
                std::string fullMsg = "PUB:" + timestamp + "[" + username + "] " + msg;
                cout << "  (公聊) " << fullMsg << endl;
                BroadcastMessage(fullMsg, clientSocket); //广播给除了自己的所有人
            }
            //解析私聊PRIV
            else if (command.find("PRIV:") == 0)
            {
                std::string content = command.substr(5); 
                size_t separatorPos = content.find(':');//第一个冒号的位置
                if (separatorPos != std::string::npos) 
                {
                    std::string targetUser = content.substr(0, separatorPos);
                    std::string msg = content.substr(separatorPos + 1);
                    std::string timestamp = getCurrentTimestamp(); //获取时间戳
                    std::string fullMsg = "PRIV:" + timestamp + "[" + username + "] " + msg;
                    cout << "  (私聊) "<< timestamp << username << "->" << targetUser << ": " << msg << endl;
                    SendPrivateMessage(fullMsg, targetUser, username); //发送私聊
                }
            }
            //解析列表请求LIST
            else if (command == "LIST:")
            {
                cout << "  (请求) " << username << " 请求了用户列表。" << endl;
                SendUserList(clientSocket);
            }
            //解析退出QUIT
            else if (command == "QUIT:") 
            {
                cout << ">>> " << username << " 主动退出了。" << endl;
                break; //跳出循环
            }
        }
        else if (recvResult == 0) 
        {
            cout << ">>> " << username << " 断开了。" << endl;
        }
        else
        {
            cout << ">>> " << username << " 接收失败: " << WSAGetLastError() << endl;
        }
    } while (recvResult > 0);

    EnterCriticalSection(&g_cs);
    g_clients.erase(clientSocket); //从map中移除
    LeaveCriticalSection(&g_cs);

    //广播xxx离开
    std::string leftMsg = "INFO:" + username + " 离开了聊天室。";
    BroadcastMessage(leftMsg, clientSocket); //广播给剩下的人

    closesocket(clientSocket);
    cout << ">>> 线程 " << clientSocket << " 服务结束。" << endl;
    return 0;
}

int main() 
{
    SetConsoleOutputCP(936);//设置控制台输出为GBK
    SetConsoleCP(936);//设置控制台输入为GBK
    cout << "--- 服务器启动 ---" << endl;

    InitializeCriticalSection(&g_cs);//初始化锁
    WSADATA wsadata;
    WSAStartup(MAKEWORD(2, 2), &wsadata);
    cout << "Winsock初始化成功" << endl;

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);//IPv4 TCP流式套接字
    cout << "Socket创建成功" << endl;

    SOCKADDR_IN serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(12345);//转换为网络字节序
    serverAddr.sin_addr.S_un.S_addr = INADDR_ANY;//监听本机所有ip
    bind(serverSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr));
    cout << "绑定成功 (端口: 12345)" << endl;

    listen(serverSocket, 5);//等待队列长度5
    cout << "开始监听 (127.0.0.1:12345)..." << endl;

    while (true)
    {
        SOCKET clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) 
        {
            cout << "Accept失败: " << WSAGetLastError() << endl;
            continue;
        }
        cout << "主线程: 接到一个新连接！ ID: " << clientSocket << endl;
        CreateThread(NULL, 0, ClientThread, (LPVOID)clientSocket, 0, NULL);
    }

    closesocket(serverSocket);
    WSACleanup();
    DeleteCriticalSection(&g_cs);
    return 0;
}