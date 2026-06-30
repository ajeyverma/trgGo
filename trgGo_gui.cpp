#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN

#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Shell32.lib")

#define BUFFER_SIZE 2048
#define TIMER_REFRESH_PEERS 101

// Control IDs
#define IDC_LISTBOX_PEERS 1001
#define IDC_BTN_CONNECT   1002
#define IDC_EDIT_CHATLOG  1003
#define IDC_EDIT_INPUT    1004
#define IDC_BTN_SEND      1005

struct Peer {
    std::string ip;
    std::string localIp;
    int port;
    std::string name;
    std::string hostname;
    std::string status;
    std::chrono::steady_clock::time_point lastSeen;
};

// Global networking variables
std::map<std::string, Peer> g_peers;
std::mutex g_peerMutex;
std::atomic<bool> g_running(true);
std::atomic<SOCKET> g_chatSocket(INVALID_SOCKET);
std::atomic<bool> g_inChat(false);
std::string g_myName;
std::string g_myHostName;
std::string g_chatPartnerName = "Peer";
int g_myTcpPort = 0;
std::string g_backendUrl = "https://trggo.vercel.app";

// Win32 Control Handles
HWND g_hWndMain = NULL;
HWND g_hListBoxPeers = NULL;
HWND g_hBtnConnect = NULL;
HWND g_hEditChatLog = NULL;
HWND g_hEditInput = NULL;
HWND g_hBtnSend = NULL;
HFONT g_hFont = NULL;

// Active keys in the ListBox
std::vector<std::string> g_listBoxKeys;

// Forward Declarations
void ReceiveMessages(SOCKET peerSocket);
SOCKET TryP2PConnect(const Peer& target);
void AppendChatLog(const std::string& sender, const std::string& text);
std::string SendHttpRequest(const std::string& method, const std::string& path, const std::string& body);

// Helper to append text to the chat log edit control (Thread-Safe via Win32 Messaging)
void AppendChatLog(const std::string& sender, const std::string& text) {
    if (g_hEditChatLog == NULL) return;
    std::string line = "[" + sender + "]: " + text + "\r\n";
    
    // Move cursor to end of text
    int len = GetWindowTextLengthA(g_hEditChatLog);
    SendMessageA(g_hEditChatLog, EM_SETSEL, len, len);
    // Insert text
    SendMessageA(g_hEditChatLog, EM_REPLACESEL, 0, (LPARAM)line.c_str());
    // Auto-scroll
    SendMessageA(g_hEditChatLog, WM_VSCROLL, SB_BOTTOM, 0);
}

// Retrieves the device's local LAN IP address
std::string GetLocalIpAddress() {
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) {
        return "127.0.0.1";
    }
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) {
        return "127.0.0.1";
    }
    std::string ip = "127.0.0.1";
    char ipStr[INET6_ADDRSTRLEN];
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_family == AF_INET6) {
            sockaddr_in6* ipv6 = (sockaddr_in6*)p->ai_addr;
            inet_ntop(AF_INET6, &ipv6->sin6_addr, ipStr, INET6_ADDRSTRLEN);
            std::string temp(ipStr);
            if (temp != "::1" && temp != "::" && temp.rfind("fe80", 0) != 0) {
                ip = temp;
                break;
            }
        } else if (p->ai_family == AF_INET) {
            sockaddr_in* ipv4 = (sockaddr_in*)p->ai_addr;
            inet_ntop(AF_INET, &ipv4->sin_addr, ipStr, INET_ADDRSTRLEN);
            std::string temp(ipStr);
            if (temp != "127.0.0.1" && temp.rfind("169.254", 0) != 0) {
                ip = temp;
            }
        }
    }
    freeaddrinfo(res);
    return ip;
}

// Executes a shell command and captures stdout
std::string ExecuteCommand(const std::string& cmd) {
    std::string result = "";
    char buffer[128];
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    _pclose(pipe);
    return result;
}

// Helper to get system DPI
int GetSystemDPI() {
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef UINT (WINAPI* GetDpiForSystemFn)();
        GetDpiForSystemFn getDpiSystem = (GetDpiForSystemFn)GetProcAddress(hUser32, "GetDpiForSystem");
        if (getDpiSystem) {
            return getDpiSystem();
        }
    }
    HDC hdc = GetDC(NULL);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);
    return dpi;
}

// Helper to get window DPI
int GetWindowDPI(HWND hWnd) {
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef UINT (WINAPI* GetDpiForWindowFn)(HWND);
        GetDpiForWindowFn getDpi = (GetDpiForWindowFn)GetProcAddress(hUser32, "GetDpiForWindow");
        if (getDpi) {
            return getDpi(hWnd);
        }
    }
    return GetSystemDPI();
}

// HTTP/HTTPS request executor using system curl
std::string SendHttpRequest(const std::string& method, const std::string& path, const std::string& body) {
    std::string fullUrl = g_backendUrl + path;
    std::string cmd = "curl -s -L -X " + method;
    
    if (!body.empty()) {
        std::string escapedBody = "";
        for (char c : body) {
            if (c == '"') escapedBody += "\\\"";
            else escapedBody += c;
        }
        cmd += " -H \"Content-Type: application/json\" -d \"" + escapedBody + "\"";
    }
    cmd += " \"" + fullUrl + "\"";
    return ExecuteCommand(cmd);
}

// Thread to periodically send HTTP heartbeats to the tracker
void HTTPHeartbeatSender() {
    std::string deviceId = g_myName + "_" + std::to_string(g_myTcpPort);
    while (g_running) {
        if (!g_inChat) {
            std::string myLocalIp = GetLocalIpAddress();
            std::string body = "{"
                               "\"deviceId\":\"" + deviceId + "\","
                               "\"hostname\":\"" + g_myHostName + "\","
                               "\"username\":\"" + g_myName + "\","
                               "\"localIp\":\"" + myLocalIp + ":" + std::to_string(g_myTcpPort) + "\""
                               "}";
            SendHttpRequest("POST", "/api/heartbeat", body);
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

// Thread to periodically fetch online peers from the tracker
void HTTPPeerFetcher() {
    while (g_running) {
        if (!g_inChat) {
            std::string response = SendHttpRequest("GET", "/api/devices", "");
            if (!response.empty()) {
                size_t pos = response.find("\"devices\":[");
                if (pos != std::string::npos) {
                    pos += 11;
                    size_t endPos = response.find("]", pos);
                    if (endPos != std::string::npos) {
                        std::string devicesArray = response.substr(pos, endPos - pos);
                        std::map<std::string, Peer> newPeers;
                        
                        size_t objStart = 0;
                        while ((objStart = devicesArray.find("{", objStart)) != std::string::npos) {
                            size_t objEnd = devicesArray.find("}", objStart);
                            if (objEnd == std::string::npos) break;
                            std::string obj = devicesArray.substr(objStart, objEnd - objStart + 1);
                            
                            auto extractField = [](const std::string& json, const std::string& field) -> std::string {
                                size_t keyPos = json.find("\"" + field + "\":");
                                if (keyPos == std::string::npos) return "";
                                keyPos += field.length() + 3;
                                if (json[keyPos] == '"') {
                                    size_t valEnd = json.find("\"", keyPos + 1);
                                    if (valEnd == std::string::npos) return "";
                                    return json.substr(keyPos + 1, valEnd - keyPos - 1);
                                } else {
                                    size_t valEnd = json.find_first_of(",}", keyPos);
                                    if (valEnd == std::string::npos) return "";
                                    return json.substr(keyPos, valEnd - keyPos);
                                }
                            };

                            std::string devName = extractField(obj, "username");
                            std::string devHost = extractField(obj, "hostname");
                            std::string devIp = extractField(obj, "publicIp");
                            std::string localIpField = extractField(obj, "localIp");
                            std::string devStatus = extractField(obj, "status");

                            std::string devLocalIp = "127.0.0.1";
                            int devPort = 0;
                            size_t lastColon = localIpField.rfind(':');
                            if (lastColon != std::string::npos) {
                                devLocalIp = localIpField.substr(0, lastColon);
                                devPort = std::stoi(localIpField.substr(lastColon + 1));
                            } else {
                                devPort = localIpField.empty() ? 0 : std::stoi(localIpField);
                            }

                            if (devIp == "::" || devIp == "::ffff:127.0.0.1" || devIp == "127.0.0.1" || devIp == "::1") {
                                devIp = "::1";
                            }
                            if (devLocalIp == "::" || devLocalIp == "::ffff:127.0.0.1" || devLocalIp == "127.0.0.1" || devLocalIp == "::1") {
                                devLocalIp = "::1";
                            }
                            // Do not show himself in list
                            if (devName == g_myName && devPort == g_myTcpPort) {
                                objStart = objEnd + 1;
                                continue;
                            }

                            if (devStatus == "online" && !devName.empty() && devPort > 0) {
                                Peer p;
                                p.ip = devIp;
                                p.localIp = devLocalIp;
                                p.port = devPort;
                                p.name = devName;
                                p.hostname = devHost;
                                p.status = devStatus;
                                p.lastSeen = std::chrono::steady_clock::now();

                                std::string key = devIp + ":" + std::to_string(devPort);
                                newPeers[key] = p;
                            }

                            objStart = objEnd + 1;
                        }

                        {
                            std::lock_guard<std::mutex> lock(g_peerMutex);
                            g_peers = newPeers;
                        }
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

// TCP Chat Server thread to accept incoming chat connections
void TCPLServer() {
    SOCKET listenSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        return;
    }

    sockaddr_in6 serverAddr = {};
    serverAddr.sin6_family = AF_INET6;
    serverAddr.sin6_addr = in6addr_any;
    serverAddr.sin6_port = 0; // Ephemeral port

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(listenSocket);
        return;
    }

    int addrLen = sizeof(serverAddr);
    if (getsockname(listenSocket, (sockaddr*)&serverAddr, &addrLen) == 0) {
        g_myTcpPort = ntohs(serverAddr.sin6_port);
    } else {
        closesocket(listenSocket);
        return;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listenSocket);
        return;
    }

    while (g_running) {
        sockaddr_in6 clientAddr = {};
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrLen);

        if (clientSocket != INVALID_SOCKET) {
            if (g_inChat) {
                closesocket(clientSocket);
            } else {
                g_chatPartnerName = "Discovered Peer";
                
                char ipStr[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, &clientAddr.sin6_addr, ipStr, INET6_ADDRSTRLEN);
                std::string clientIp(ipStr);
                if (clientIp == "::") clientIp = "::1";

                {
                    std::lock_guard<std::mutex> lock(g_peerMutex);
                    for (const auto& pair : g_peers) {
                        if (pair.second.ip == clientIp || pair.second.localIp == clientIp) {
                            g_chatPartnerName = pair.second.name;
                            break;
                        }
                    }
                }

                // Append alert to UI
                AppendChatLog("System", "Incoming connection accepted from " + g_chatPartnerName + "!");
                
                g_chatSocket = clientSocket;
                g_inChat = true;

                // Disable Connect and Enable Chat controls
                EnableWindow(g_hBtnConnect, FALSE);
                EnableWindow(g_hEditInput, TRUE);
                EnableWindow(g_hBtnSend, TRUE);

                // Spawn message receiver thread
                std::thread recvThread(ReceiveMessages, g_chatSocket.load());
                recvThread.detach();
            }
        }
    }

    closesocket(listenSocket);
}

// Receive loop for peer messages
void ReceiveMessages(SOCKET peerSocket) {
    char buffer[BUFFER_SIZE];
    while (g_inChat && g_running) {
        int bytesReceived = recv(peerSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            AppendChatLog(g_chatPartnerName, buffer);
        } else if (bytesReceived == 0) {
            AppendChatLog("System", "Connection closed by peer.");
            g_inChat = false;
            break;
        } else {
            int error = WSAGetLastError();
            if (error != WSAEINTR && error != WSAENOTSOCK) {
                AppendChatLog("System", "Connection lost (Error: " + std::to_string(error) + ").");
            }
            g_inChat = false;
            break;
        }
    }

    // Reset UI state
    EnableWindow(g_hBtnConnect, TRUE);
    EnableWindow(g_hEditInput, FALSE);
    EnableWindow(g_hBtnSend, FALSE);
}

// Unified client connection helper (Tries LAN, falls back to WAN)
SOCKET TryP2PConnect(const Peer& target) {
    bool isLocalIPv6 = (target.localIp.find(':') != std::string::npos);
    AppendChatLog("System", "Trying LAN connection to [" + target.localIp + "]:" + std::to_string(target.port) + "...");
    
    SOCKET sock = socket(isLocalIPv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock != INVALID_SOCKET) {
        bool connected = false;
        if (isLocalIPv6) {
            sockaddr_in6 addr = {};
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(target.port);
            inet_pton(AF_INET6, target.localIp.c_str(), &addr.sin6_addr);
            if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR) {
                connected = true;
            }
        } else {
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(target.port);
            inet_pton(AF_INET, target.localIp.c_str(), &addr.sin_addr);
            if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR) {
                connected = true;
            }
        }

        if (connected) {
            AppendChatLog("System", "LAN connection successful!");
            return sock;
        }
        closesocket(sock);
    }

    bool isPublicIPv6 = (target.ip.find(':') != std::string::npos);
    AppendChatLog("System", "LAN failed. Trying WAN connection to [" + target.ip + "]:" + std::to_string(target.port) + "...");
    
    sock = socket(isPublicIPv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock != INVALID_SOCKET) {
        bool connected = false;
        if (isPublicIPv6) {
            sockaddr_in6 addr = {};
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(target.port);
            inet_pton(AF_INET6, target.ip.c_str(), &addr.sin6_addr);
            if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR) {
                connected = true;
            }
        } else {
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(target.port);
            inet_pton(AF_INET, target.ip.c_str(), &addr.sin_addr);
            if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR) {
                connected = true;
            }
        }

        if (connected) {
            AppendChatLog("System", "WAN connection successful!");
            return sock;
        }
        closesocket(sock);
    }

    return INVALID_SOCKET;
}

// Background thread to handle peer connection without locking UI message loop
void ConnectPeerAsync(Peer target) {
    SOCKET connSocket = TryP2PConnect(target);
    if (connSocket != INVALID_SOCKET) {
        g_chatPartnerName = target.name;
        g_chatSocket = connSocket;
        g_inChat = true;

        EnableWindow(g_hBtnConnect, FALSE);
        EnableWindow(g_hEditInput, TRUE);
        EnableWindow(g_hBtnSend, TRUE);

        std::thread recvThread(ReceiveMessages, g_chatSocket.load());
        recvThread.detach();
    } else {
        AppendChatLog("System", "Connection failed to both LAN and WAN addresses.");
        EnableWindow(g_hBtnConnect, TRUE);
    }
}

// Send Message Handler
void HandleSend() {
    char inputBuf[1024];
    GetWindowTextA(g_hEditInput, inputBuf, sizeof(inputBuf));
    std::string msg(inputBuf);
    
    if (msg.empty()) return;

    if (msg == "/exit") {
        AppendChatLog("System", "Disconnecting session...");
        g_inChat = false;
        shutdown(g_chatSocket, SD_BOTH);
        closesocket(g_chatSocket);
        g_chatSocket = INVALID_SOCKET;
        SetWindowTextA(g_hEditInput, "");
        return;
    }

    if (g_inChat && g_chatSocket != INVALID_SOCKET) {
        int bytesSent = send(g_chatSocket, msg.c_str(), (int)msg.length(), 0);
        if (bytesSent != SOCKET_ERROR) {
            AppendChatLog("You", msg);
            SetWindowTextA(g_hEditInput, "");
        } else {
            AppendChatLog("System", "Send failed. Error: " + std::to_string(WSAGetLastError()));
        }
    }
}

// Win32 Windows Event Handler Procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            int dpi = GetWindowDPI(hWnd);
            auto scale = [dpi](int val) { return MulDiv(val, dpi, 96); };

            int fontHeight = -MulDiv(11, dpi, 72); // 11pt font size
            g_hFont = CreateFontA(
                fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI"
            );

            // Left Panel: Online Peers
            HWND hStatic1 = CreateWindowA("STATIC", "Online Peers:", WS_VISIBLE | WS_CHILD,
                         scale(10), scale(10), scale(180), scale(20), hWnd, NULL, NULL, NULL);
            if (hStatic1 && g_hFont) SendMessageA(hStatic1, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            
            g_hListBoxPeers = CreateWindowA("LISTBOX", NULL, 
                                          WS_VISIBLE | WS_CHILD | LBS_NOTIFY | WS_VSCROLL | WS_BORDER,
                                          scale(10), scale(35), scale(180), scale(280), hWnd, (HMENU)IDC_LISTBOX_PEERS, NULL, NULL);
            if (g_hListBoxPeers && g_hFont) SendMessageA(g_hListBoxPeers, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            g_hBtnConnect = CreateWindowA("BUTTON", "Connect", 
                                         WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                                         scale(10), scale(325), scale(180), scale(30), hWnd, (HMENU)IDC_BTN_CONNECT, NULL, NULL);
            if (g_hBtnConnect && g_hFont) SendMessageA(g_hBtnConnect, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Right Panel: Chat Log
            HWND hStatic2 = CreateWindowA("STATIC", "Conversation history:", WS_VISIBLE | WS_CHILD,
                         scale(200), scale(10), scale(370), scale(20), hWnd, NULL, NULL, NULL);
            if (hStatic2 && g_hFont) SendMessageA(hStatic2, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            g_hEditChatLog = CreateWindowA("EDIT", NULL,
                                          WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_BORDER,
                                          scale(200), scale(35), scale(370), scale(240), hWnd, (HMENU)IDC_EDIT_CHATLOG, NULL, NULL);
            if (g_hEditChatLog && g_hFont) SendMessageA(g_hEditChatLog, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            // Input fields
            g_hEditInput = CreateWindowA("EDIT", NULL,
                                        WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | WS_BORDER,
                                        scale(200), scale(285), scale(280), scale(30), hWnd, (HMENU)IDC_EDIT_INPUT, NULL, NULL);
            if (g_hEditInput && g_hFont) SendMessageA(g_hEditInput, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            EnableWindow(g_hEditInput, FALSE);

            g_hBtnSend = CreateWindowA("BUTTON", "Send",
                                      WS_VISIBLE | WS_CHILD,
                                      scale(490), scale(285), scale(80), scale(30), hWnd, (HMENU)IDC_BTN_SEND, NULL, NULL);
            if (g_hBtnSend && g_hFont) SendMessageA(g_hBtnSend, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            EnableWindow(g_hBtnSend, FALSE);

            // Print info
            AppendChatLog("System", "Welcome to trgGo " + g_myName + "!");
            AppendChatLog("System", "Port allocated: " + std::to_string(g_myTcpPort));
            AppendChatLog("System", "Double-click a peer and connect to start chatting.");

            // Set timers to periodically refresh peer list from memory
            SetTimer(hWnd, TIMER_REFRESH_PEERS, 3000, NULL);
            break;
        }

        case WM_TIMER: {
            if (wParam == TIMER_REFRESH_PEERS) {
                // Keep selected index
                int selIdx = (int)SendMessageA(g_hListBoxPeers, LB_GETCURSEL, 0, 0);
                
                // Clear ListBox
                SendMessageA(g_hListBoxPeers, LB_RESETCONTENT, 0, 0);
                g_listBoxKeys.clear();

                std::lock_guard<std::mutex> lock(g_peerMutex);
                for (const auto& pair : g_peers) {
                    std::string display = pair.second.name + " (" + pair.second.hostname + ")";
                    SendMessageA(g_hListBoxPeers, LB_ADDSTRING, 0, (LPARAM)display.c_str());
                    g_listBoxKeys.push_back(pair.first);
                }

                // Restore selection if valid
                if (selIdx != LB_ERR && selIdx < (int)g_listBoxKeys.size()) {
                    SendMessageA(g_hListBoxPeers, LB_SETCURSEL, selIdx, 0);
                }
            }
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            int wmEvent = HIWORD(wParam);

            if (wmId == IDC_BTN_CONNECT && wmEvent == BN_CLICKED) {
                int selIdx = (int)SendMessageA(g_hListBoxPeers, LB_GETCURSEL, 0, 0);
                if (selIdx != LB_ERR && selIdx < (int)g_listBoxKeys.size()) {
                    std::string key = g_listBoxKeys[selIdx];
                    Peer target;
                    {
                        std::lock_guard<std::mutex> lock(g_peerMutex);
                        target = g_peers[key];
                    }

                    if (target.name == g_myName && target.port == g_myTcpPort) {
                        MessageBoxA(hWnd, "You cannot connect to yourself!", "Warning", MB_OK | MB_ICONWARNING);
                    } else {
                        EnableWindow(g_hBtnConnect, FALSE);
                        std::thread connThread(ConnectPeerAsync, target);
                        connThread.detach();
                    }
                } else {
                    MessageBoxA(hWnd, "Please select a peer from the list first.", "Information", MB_OK | MB_ICONINFORMATION);
                }
            }
            
            if (wmId == IDC_BTN_SEND && wmEvent == BN_CLICKED) {
                HandleSend();
            }

            // Capture enter key inside edit control
            if (wmId == IDC_EDIT_INPUT && wmEvent == EN_CHANGE) {
                // Simple subclassing or checking message queue is standard, but for this basic app
                // we can capture Enter by adding default push buttons, or standard command handler.
            }
            break;
        }

        case WM_DESTROY: {
            g_running = false;
            KillTimer(hWnd, TIMER_REFRESH_PEERS);
            
            // Cleanup active font
            if (g_hFont != NULL) {
                DeleteObject(g_hFont);
                g_hFont = NULL;
            }

            // Cleanup active sockets
            SOCKET cs = g_chatSocket.exchange(INVALID_SOCKET);
            if (cs != INVALID_SOCKET) {
                closesocket(cs);
            }

            #ifdef _WIN32
                WSACleanup();
            #endif

            PostQuitMessage(0);
            break;
        }
        default:
            return DefWindowProcA(hWnd, message, wParam, lParam);
    }
    return 0;
}

int main() {
    // Set process DPI awareness to prevent blurriness on high-DPI displays
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI* SetProcessDpiAwarenessContextFn)(HANDLE);
        SetProcessDpiAwarenessContextFn setDpiCtx = (SetProcessDpiAwarenessContextFn)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiCtx) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is (HANDLE)-4
            setDpiCtx((HANDLE)-4);
        } else {
            SetProcessDPIAware();
        }
    } else {
        SetProcessDPIAware();
    }

    // 1. Ask for Nickname and Tracker URL in the console window first
    std::cout << "===========================================\n";
    std::cout << "        trgGo - Native Windows GUI         \n";
    std::cout << "===========================================\n";
    std::cout << "Enter your Nickname: ";
    std::getline(std::cin, g_myName);
    if (g_myName.empty()) {
        g_myName = "AnonymousPeer";
    }

    std::cout << "Enter Tracker Base URL (default https://trggo.vercel.app): ";
    std::string urlInput;
    std::getline(std::cin, urlInput);
    if (!urlInput.empty()) {
        g_backendUrl = urlInput;
    }

    if (g_backendUrl.rfind("http", 0) != 0) {
        g_backendUrl = "https://" + g_backendUrl;
    }
    if (!g_backendUrl.empty() && g_backendUrl.back() == '/') {
        g_backendUrl.pop_back();
    }

    // Initialize Winsock
    WSADATA wsaData;
    int wsResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsResult != 0) {
        std::cerr << "Winsock initialization failed.\n";
        return 1;
    }

    // Retrieve local hostname
    char hostNameBuf[256];
    g_myHostName = "trgGo-Node";
    if (gethostname(hostNameBuf, sizeof(hostNameBuf)) == 0) {
        g_myHostName = hostNameBuf;
    }

    // 2. Start TCP Server & Background Threads
    std::thread tcpServerThread(TCPLServer);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::thread heartbeatThread(HTTPHeartbeatSender);
    std::thread fetcherThread(HTTPPeerFetcher);

    // Hide console window once input is captured (if desired)
    // ShowWindow(GetConsoleWindow(), SW_HIDE);
    std::cout << "[System]: Starting native Win32 window...\n";

    // 3. Register and Create Win32 Main GUI Window
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASSEXA wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXA);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = "trgGoGuiClass";

    if (!RegisterClassExA(&wcex)) {
        std::cerr << "Failed to register window class.\n";
        g_running = false;
        heartbeatThread.join();
        fetcherThread.join();
        tcpServerThread.join();
        WSACleanup();
        return 1;
    }

    // Center window on screen
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    // Scale window dimensions based on DPI
    int systemDpi = GetSystemDPI();
    int winWidth = MulDiv(600, systemDpi, 96);
    int winHeight = MulDiv(400, systemDpi, 96);
    
    int winX = (screenWidth - winWidth) / 2;
    int winY = (screenHeight - winHeight) / 2;

    std::string winTitle = "trgGo - " + g_backendUrl;
    g_hWndMain = CreateWindowExA(
        0, "trgGoGuiClass", winTitle.c_str(),
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, // Fixed size window
        winX, winY, winWidth, winHeight,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hWndMain) {
        std::cerr << "Failed to create main window.\n";
        g_running = false;
        heartbeatThread.join();
        fetcherThread.join();
        tcpServerThread.join();
        WSACleanup();
        return 1;
    }

    ShowWindow(g_hWndMain, SW_SHOW);
    UpdateWindow(g_hWndMain);

    // Win32 Message Loop Dispatcher
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        // Capture enter key inside edit control to trigger send button
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            if (GetFocus() == g_hEditInput) {
                HandleSend();
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup background threads
    g_running = false;
    if (heartbeatThread.joinable()) heartbeatThread.join();
    if (fetcherThread.joinable()) fetcherThread.join();
    if (tcpServerThread.joinable()) tcpServerThread.join();

    std::cout << "[System]: Exit successful.\n";
    return 0;
}
