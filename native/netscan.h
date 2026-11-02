#pragma once
// 局域网 5555 端口扫描：无线 ADB 的 IP 变化后用于重新发现设备
#include <string>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

namespace netscan
{
inline void EnsureWsa()
{
    static bool done = false;
    if (!done)
    {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return;   // 失败不标记完成，下次再试
        done = true;
    }
}

// 以 hintIp 的前三段为网段，扫描 1..254 的 5555 端口
// timeoutMs 为整体等待时间，并行连接所以很快
inline std::vector<std::string> ScanAdbPorts(const std::string& hintIp, int timeoutMs = 400)
{
    EnsureWsa();

    std::vector<std::string> found;
    size_t dot = hintIp.rfind('.');
    if (dot == std::string::npos) return found;
    std::string prefix = hintIp.substr(0, dot + 1);

    // 注意：Winsock 的 fd_set 上限是 64（FD_SETSIZE），必须分批
    const int kBatch = 60;
    int perBatchMs = timeoutMs * kBatch / 254;
    if (perBatchMs < 120) perBatchMs = 120;

    for (int base = 1; base <= 254; base += kBatch)
    {
        struct Conn { SOCKET s; int host; };
        std::vector<Conn> conns;
        conns.reserve(kBatch);

        for (int h = base; h < base + kBatch && h <= 254; h++)
        {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) continue;

            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);

            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(5555);
            std::string ip = prefix + std::to_string(h);
            if (InetPtonA(AF_INET, ip.c_str(), &a.sin_addr) != 1) { closesocket(s); continue; }

            connect(s, (sockaddr*)&a, sizeof(a));   // 非阻塞：立即返回
            conns.push_back({ s, h });
        }

        if (conns.empty()) continue;

        fd_set wr, ex;
        FD_ZERO(&wr); FD_ZERO(&ex);
        for (auto& c : conns) { FD_SET(c.s, &wr); FD_SET(c.s, &ex); }

        timeval tv{ perBatchMs / 1000, (perBatchMs % 1000) * 1000 };
        select(0, nullptr, &wr, &ex, &tv);

        for (auto& c : conns)
        {
            if (FD_ISSET(c.s, &wr))
            {
                int err = 0; int len = sizeof(err);
                if (getsockopt(c.s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0)
                    found.push_back(prefix + std::to_string(c.host));
            }
            closesocket(c.s);
        }
    }
    return found;
}
}
