#ifndef UNICODE
#define UNICODE
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
#include <iomanip>

#ifdef _WIN32
    #include <conio.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <ws2ipdef.h>
    // Link with Ws2_32.lib
    #pragma comment(lib, "Ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <sys/ioctl.h>
    #include <termios.h>
    #include <sys/select.h>
    #include <errno.h>

    typedef int SOCKET;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define SD_BOTH SHUT_RDWR
    #define closesocket close
    #define WSAGetLastError() errno
    #define WSAEINTR EINTR
    #define WSAENOTSOCK EBADF
#endif

#define BUFFER_SIZE 2048

// Color namespace for beautiful terminal styling
namespace color {
    const char* RESET   = "\033[0m";
    const char* BOLD    = "\033[1m";
    const char* CYAN    = "\033[36m";
    const char* GREEN   = "\033[32m";
    const char* RED     = "\033[31m";
}

// Column widths for the device table
const int W_ID   = 4;
const int W_NAME = 16;
const int W_USER = 16;
const int W_IP   = 48;
const int W_STAT = 8;

struct Peer {
    std::string ip;
    int port;
    std::string name;       // User
    std::string hostname;   // Name (Host)
    std::string status;     // "online" or "offline"
    std::chrono::steady_clock::time_point lastSeen;
};

// Global variables
std::map<std::string, Peer> g_peers;
std::mutex g_peerMutex;
std::atomic<bool> g_running(true);
std::atomic<SOCKET> g_chatSocket(INVALID_SOCKET);
std::atomic<bool> g_inChat(false);
std::atomic<bool> g_needRedraw(true);
std::string g_myName;
std::string g_myHostName;
std::string g_chatPartnerName = "Peer";
int g_myTcpPort = 0;

std::string g_backendIp;
int g_backendPort = 8000;

std::vector<std::string> g_chatHistory;
std::mutex g_chatMutex;
std::atomic<bool> g_chatNeedRedraw(true);

#ifndef _WIN32
// POSIX kbhit and getch implementation for Linux/Termux/macOS
int _kbhit() {
    static const int STDIN = 0;
    static bool initialized = false;

    if (!initialized) {
        termios term;
        tcgetattr(STDIN, &term);
        term.c_lflag &= ~ICANON;
        term.c_lflag &= ~ECHO;
        tcsetattr(STDIN, TCSANOW, &term);
        initialized = true;
    }

    int bytesWaiting;
    ioctl(STDIN, FIONREAD, &bytesWaiting);
    return bytesWaiting;
}

int _getch() {
    char ch;
    if (read(0, &ch, 1) < 0) return 0;
    return ch;
}
#endif

// Cross-platform Screen Clear using ANSI escape sequences
void ClearScreen() {
    std::cout << "\033[2J\033[1;1H" << std::flush;
}

// Raw C++ HTTP Request client (Supports both IPv4 and IPv6)
std::string SendHttpRequest(const std::string& method, const std::string& path, const std::string& body) {
    bool isIPv6 = (g_backendIp.find(':') != std::string::npos);
    SOCKET sock = socket(isIPv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return "";
    }

    if (isIPv6) {
        sockaddr_in6 serverAddr = {};
        serverAddr.sin6_family = AF_INET6;
        serverAddr.sin6_port = htons(g_backendPort);
        if (inet_pton(AF_INET6, g_backendIp.c_str(), &serverAddr.sin6_addr) != 1) {
            closesocket(sock);
            return "";
        }
        if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            closesocket(sock);
            return "";
        }
    } else {
        sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(g_backendPort);
        if (inet_pton(AF_INET, g_backendIp.c_str(), &serverAddr.sin_addr) != 1) {
            closesocket(sock);
            return "";
        }
        if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            closesocket(sock);
            return "";
        }
    }

    std::string hostHeader = g_backendIp;
    if (isIPv6) {
        hostHeader = "[" + g_backendIp + "]";
    }

    std::string request = method + " " + path + " HTTP/1.1\r\n" +
                          "Host: " + hostHeader + ":" + std::to_string(g_backendPort) + "\r\n" +
                          "Content-Type: application/json\r\n" +
                          "Content-Length: " + std::to_string(body.length()) + "\r\n" +
                          "Connection: close\r\n\r\n" +
                          body;

    send(sock, request.c_str(), (int)request.length(), 0);

    std::string response = "";
    char buffer[1024];
    int bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        response += buffer;
    }

    closesocket(sock);
    return response;
}

// Thread to periodically send HTTP heartbeats to the tracker
void HTTPHeartbeatSender() {
    std::string deviceId = g_myName + "_" + std::to_string(g_myTcpPort);
    while (g_running) {
        if (!g_inChat) {
            std::string body = "{"
                               "\"deviceId\":\"" + deviceId + "\","
                               "\"hostname\":\"" + g_myHostName + "\","
                               "\"username\":\"" + g_myName + "\","
                               "\"localIp\":\"" + std::to_string(g_myTcpPort) + "\""
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
                            std::string portStr = extractField(obj, "localIp");
                            std::string devStatus = extractField(obj, "status");

                            int devPort = portStr.empty() ? 0 : std::stoi(portStr);

                            // Resolve IPv6 local loopback formats
                            if (devIp == "::" || devIp == "::ffff:127.0.0.1" || devIp == "127.0.0.1") {
                                devIp = "::1";
                            }

                            // Skip ourselves
                            if (devName == g_myName && devPort == g_myTcpPort) {
                                objStart = objEnd + 1;
                                continue;
                            }

                            // Add online devices to active peer map
                            if (devStatus == "online" && !devName.empty() && devPort > 0) {
                                Peer p;
                                p.ip = devIp;
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

                        // Determine if list changed to avoid flickering
                        bool changed = false;
                        {
                            std::lock_guard<std::mutex> lock(g_peerMutex);
                            if (g_peers.size() != newPeers.size()) {
                                changed = true;
                            } else {
                                for (const auto& pair : newPeers) {
                                    if (g_peers.find(pair.first) == g_peers.end()) {
                                        changed = true;
                                        break;
                                    }
                                }
                            }
                            g_peers = newPeers;
                        }

                        if (changed) {
                            g_needRedraw = true;
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

    #ifdef _WIN32
        int addrLen = sizeof(serverAddr);
    #else
        socklen_t addrLen = sizeof(serverAddr);
    #endif
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
        #ifdef _WIN32
            int clientAddrLen = sizeof(clientAddr);
        #else
            socklen_t clientAddrLen = sizeof(clientAddr);
        #endif
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
                        if (pair.second.ip == clientIp) {
                            g_chatPartnerName = pair.second.name;
                            break;
                        }
                    }
                }

                g_chatSocket = clientSocket;
                g_inChat = true;
            }
        }
    }

    closesocket(listenSocket);
}

// Thread to receive messages in a chat session
void ReceiveMessages(SOCKET peerSocket) {
    char buffer[BUFFER_SIZE];
    while (g_inChat && g_running) {
        int bytesReceived = recv(peerSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            {
                std::lock_guard<std::mutex> lock(g_chatMutex);
                g_chatHistory.push_back("[" + g_chatPartnerName + "]: " + std::string(buffer));
            }
            g_chatNeedRedraw = true;
        } else if (bytesReceived == 0) {
            {
                std::lock_guard<std::mutex> lock(g_chatMutex);
                g_chatHistory.push_back("[System]: Connection closed by peer.");
            }
            g_chatNeedRedraw = true;
            g_inChat = false;
            break;
        } else {
            int error = WSAGetLastError();
            if (error != WSAEINTR && error != WSAENOTSOCK) {
                std::lock_guard<std::mutex> lock(g_chatMutex);
                g_chatHistory.push_back("[System]: Connection lost (Error: " + std::to_string(error) + ").");
            }
            g_chatNeedRedraw = true;
            g_inChat = false;
            break;
        }
    }
}

int main() {
    #ifdef _WIN32
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup failed with error: " << result << "\n";
            return 1;
        }

        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD dwMode = 0;
            if (GetConsoleMode(hOut, &dwMode)) {
                dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                SetConsoleMode(hOut, dwMode);
            }
        }
    #endif

    char hostNameBuf[256];
    g_myHostName = "trgGo-Node";
    if (gethostname(hostNameBuf, sizeof(hostNameBuf)) == 0) {
        g_myHostName = hostNameBuf;
    }

    ClearScreen();
    std::cout << "===========================================\n";
    std::cout << "   trgGo - IPv6 P2P Chat with Discovery    \n";
    std::cout << "===========================================\n";
    std::cout << "Enter your Nickname: ";
    std::getline(std::cin, g_myName);
    if (g_myName.empty()) {
        g_myName = "AnonymousPeer";
    }

    std::cout << "Enter Tracker IP (default 127.0.0.1): ";
    std::string ipInput;
    std::getline(std::cin, ipInput);
    g_backendIp = ipInput.empty() ? "127.0.0.1" : ipInput;

    std::cout << "Enter Tracker Port (default 8000): ";
    std::string portInput;
    std::getline(std::cin, portInput);
    g_backendPort = portInput.empty() ? 8000 : std::stoi(portInput);

    // 2. Start TCP Chat Server (assigns ephemeral port)
    std::thread tcpServerThread(TCPLServer);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 3. Start HTTP Heartbeat Sender & online peer fetcher
    std::thread heartbeatThread(HTTPHeartbeatSender);
    std::thread fetcherThread(HTTPPeerFetcher);

    std::vector<Peer> activePeersList;
    std::string currentInput = "";

    // Helper lambda to draw table line
    auto hline = [&]() {
        std::cout << "  +"
                  << std::string(W_ID   + 2, '-') << "+"
                  << std::string(W_NAME + 2, '-') << "+"
                  << std::string(W_USER + 2, '-') << "+"
                  << std::string(W_IP   + 2, '-') << "+"
                  << std::string(W_STAT + 2, '-') << "+\n";
    };

    auto print_separator = [&]() {
        std::cout << "  " << std::string(W_ID + W_NAME + W_USER + W_IP + W_STAT + 17, '=') << "\n";
    };

    auto print_dash_separator = [&]() {
        std::cout << "  " << std::string(W_ID + W_NAME + W_USER + W_IP + W_STAT + 17, '-') << "\n";
    };

    while (g_running) {
        if (g_inChat && g_chatSocket != INVALID_SOCKET) {
            goto enter_chat_room;
        }

        if (g_needRedraw) {
            g_needRedraw = false;
            ClearScreen();

            print_separator();
            std::cout << "  " << color::CYAN << color::BOLD << "trgGo P2P Console" << color::RESET 
                      << " | User: " << color::GREEN << g_myName << color::RESET 
                      << " | Host: " << g_myHostName 
                      << " | Port: " << g_myTcpPort << "\n";
            print_separator();

            hline();
            std::cout << color::BOLD << color::CYAN
                      << "  | " << std::setw(W_ID)   << std::left << "ID"
                      << " | " << std::setw(W_NAME) << std::left << "Name (Host)"
                      << " | " << std::setw(W_USER) << std::left << "User"
                      << " | " << std::setw(W_IP)   << std::left << "Public IP"
                      << " | " << std::setw(W_STAT) << std::left << "Status"
                      << " |" << color::RESET << "\n";
            hline();

            activePeersList.clear();
            {
                std::lock_guard<std::mutex> lock(g_peerMutex);
                int idx = 1;
                for (const auto& pair : g_peers) {
                    activePeersList.push_back(pair.second);
                    
                    const char* sc = (pair.second.status == "online") ? color::GREEN : color::RED;
                    std::string displayIp = pair.second.ip + ":" + std::to_string(pair.second.port);

                    std::cout << "  | " << color::BOLD << std::setw(W_ID) << std::left << idx++ << color::RESET
                              << " | " << std::setw(W_NAME) << std::left << pair.second.hostname
                              << " | " << std::setw(W_USER) << std::left << pair.second.name
                              << " | " << std::setw(W_IP)   << std::left << displayIp
                              << " | " << sc << std::setw(W_STAT) << std::left << pair.second.status << color::RESET
                              << " |\n";
                }
            }

            if (activePeersList.empty()) {
                std::cout << "  | " << std::setw(W_ID + W_NAME + W_USER + W_IP + W_STAT + 12) << std::left 
                          << "   No devices online yet. Searching..." << " |\n";
            }
            hline();

            std::cout << "\nCommands: engage <id>, initiate <id>, use <id>, or raw <id>\n";
            std::cout << "Type /exit to quit.\n";
            print_dash_separator();
            std::cout << "> " << currentInput << std::flush;
        }

        if (_kbhit()) {
            int ch = _getch();
            if (ch == 224 || ch == 0) {
                _getch();
                continue;
            }

            if (ch == 8 || ch == 127) { // Backspace
                if (!currentInput.empty()) {
                    currentInput.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
            } else if (ch == 13 || ch == 10) { // Enter
                std::cout << "\n";
                std::string cmd = currentInput;
                currentInput = "";

                if (cmd == "/exit") {
                    g_running = false;
                    break;
                }

                std::string targetIdStr = "";
                if (cmd.rfind("engage ", 0) == 0 && cmd.length() > 7) {
                    targetIdStr = cmd.substr(7);
                } else if (cmd.rfind("initiate ", 0) == 0 && cmd.length() > 9) {
                    targetIdStr = cmd.substr(9);
                } else if (cmd.rfind("use ", 0) == 0 && cmd.length() > 4) {
                    targetIdStr = cmd.substr(4);
                } else {
                    targetIdStr = cmd;
                }

                while (!targetIdStr.empty() && targetIdStr.front() == ' ') targetIdStr.erase(0, 1);
                while (!targetIdStr.empty() && targetIdStr.back() == ' ') targetIdStr.pop_back();

                if (!targetIdStr.empty()) {
                    try {
                        int selection = std::stoi(targetIdStr);
                        if (selection >= 1 && selection <= (int)activePeersList.size()) {
                            Peer target = activePeersList[selection - 1];
                            std::cout << "[System]: Connecting to " << target.name << " at [" << target.ip << "]:" << target.port << "...\n";

                            // Determine if connecting IP is IPv6 or IPv4
                            bool isTargetIPv6 = (target.ip.find(':') != std::string::npos);
                            SOCKET connectSocket = socket(isTargetIPv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
                            
                            if (connectSocket != INVALID_SOCKET) {
                                bool connected = false;
                                if (isTargetIPv6) {
                                    sockaddr_in6 peerAddr = {};
                                    peerAddr.sin6_family = AF_INET6;
                                    peerAddr.sin6_port = htons(target.port);
                                    inet_pton(AF_INET6, target.ip.c_str(), &peerAddr.sin6_addr);
                                    if (connect(connectSocket, (sockaddr*)&peerAddr, sizeof(peerAddr)) != SOCKET_ERROR) {
                                        connected = true;
                                    }
                                } else {
                                    sockaddr_in peerAddr = {};
                                    peerAddr.sin_family = AF_INET;
                                    peerAddr.sin_port = htons(target.port);
                                    inet_pton(AF_INET, target.ip.c_str(), &peerAddr.sin_addr);
                                    if (connect(connectSocket, (sockaddr*)&peerAddr, sizeof(peerAddr)) != SOCKET_ERROR) {
                                        connected = true;
                                    }
                                }

                                if (connected) {
                                    g_chatPartnerName = target.name;
                                    g_chatSocket = connectSocket;
                                    g_inChat = true;
                                } else {
                                    std::cerr << "[System]: Connection failed. Error: " << WSAGetLastError() << "\n";
                                    closesocket(connectSocket);
                                    std::this_thread::sleep_for(std::chrono::seconds(2));
                                    g_needRedraw = true;
                                }
                            }
                        } else {
                            std::cout << "[System]: Invalid peer number.\n";
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            g_needRedraw = true;
                        }
                    } catch (...) {
                        std::cout << "[System]: Unknown command or invalid selection.\n";
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        g_needRedraw = true;
                    }
                } else {
                    g_needRedraw = true;
                }
            } else if (ch >= 32 && ch <= 126) {
                currentInput += (char)ch;
                std::cout << (char)ch << std::flush;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

    enter_chat_room:
        if (g_inChat && g_chatSocket != INVALID_SOCKET) {
            {
                std::lock_guard<std::mutex> lock(g_chatMutex);
                g_chatHistory.clear();
            }
            g_chatNeedRedraw = true;
            std::string currentChatInput = "";

            std::thread recvThread(ReceiveMessages, g_chatSocket.load());

            while (g_inChat && g_running) {
                if (g_chatNeedRedraw) {
                    g_chatNeedRedraw = false;
                    ClearScreen();

                    print_separator();
                    std::cout << "  " << color::CYAN << color::BOLD << "LIVE P2P CHAT SESSION" << color::RESET 
                              << " | Peer: " << color::GREEN << color::BOLD << g_chatPartnerName << color::RESET 
                              << " | Connection: " << color::GREEN << "Established" << color::RESET << "\n";
                    std::cout << "  " << "Instruction: Type messages below and press Enter. Send " << color::RED << "/exit" << color::RESET << " to disconnect.\n";
                    print_separator();
                    std::cout << "\n";

                    {
                        std::lock_guard<std::mutex> lock(g_chatMutex);
                        for (const auto& line : g_chatHistory) {
                            std::cout << line << "\n";
                        }
                    }
                    std::cout << "[You]: " << currentChatInput << std::flush;
                }

                if (_kbhit()) {
                    int ch = _getch();
                    if (ch == 224 || ch == 0) {
                        _getch();
                        continue;
                    }

                    if (ch == 8 || ch == 127) { // Backspace
                        if (!currentChatInput.empty()) {
                            currentChatInput.pop_back();
                            std::cout << "\b \b" << std::flush;
                        }
                    } else if (ch == 13 || ch == 10) { // Enter
                        std::cout << "\n";
                        std::string msg = currentChatInput;
                        currentChatInput = "";

                        if (msg == "/exit") {
                            g_inChat = false;
                            break;
                        }

                        if (!msg.empty()) {
                            int bytesSent = send(g_chatSocket, msg.c_str(), (int)msg.length(), 0);
                            if (bytesSent == SOCKET_ERROR) {
                                std::lock_guard<std::mutex> lock(g_chatMutex);
                                g_chatHistory.push_back("[System]: Send failed. Error: " + std::to_string(WSAGetLastError()));
                                g_inChat = false;
                                break;
                            }
                            {
                                std::lock_guard<std::mutex> lock(g_chatMutex);
                                g_chatHistory.push_back("[You]: " + msg);
                            }
                            g_chatNeedRedraw = true;
                        } else {
                            g_chatNeedRedraw = true;
                        }
                    } else if (ch >= 32 && ch <= 126) {
                        currentChatInput += (char)ch;
                        std::cout << (char)ch << std::flush;
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }

            shutdown(g_chatSocket, SD_BOTH);
            closesocket(g_chatSocket);
            g_chatSocket = INVALID_SOCKET;
            g_inChat = false;

            if (recvThread.joinable()) {
                recvThread.join();
            }

            std::cout << "\n[System]: Returning to online peer list...\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
            g_needRedraw = true;
        }
    }

    g_running = false;
    
    SOCKET cs = g_chatSocket.exchange(INVALID_SOCKET);
    if (cs != INVALID_SOCKET) {
        closesocket(cs);
    }

    if (tcpServerThread.joinable()) tcpServerThread.join();
    if (heartbeatThread.joinable()) heartbeatThread.join();
    if (fetcherThread.joinable()) fetcherThread.join();

    #ifdef _WIN32
        WSACleanup();
    #endif
    std::cout << "[System]: Closed. Goodbye!\n";
    return 0;
}
