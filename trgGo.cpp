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
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>

#pragma comment(lib, "Ws2_32.lib")

#define DISCOVERY_PORT 9999
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
const int W_IP   = 22;
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

// Prune peers that haven't sent a beacon in 6 seconds
void PruneOfflinePeers() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (g_inChat) continue;

        auto now = std::chrono::steady_clock::now();
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_peerMutex);
            for (auto it = g_peers.begin(); it != g_peers.end(); ) {
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastSeen).count();
                if (duration > 6) {
                    it = g_peers.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
        if (changed) {
            g_needRedraw = true;
        }
    }
}

// Thread to periodically send UDP multicast beacons
void UDPBeaconSender() {
    SOCKET udpSocket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        return;
    }

    sockaddr_in6 multicastAddr = {};
    multicastAddr.sin6_family = AF_INET6;
    multicastAddr.sin6_port = htons(DISCOVERY_PORT);
    inet_pton(AF_INET6, "ff02::1", &multicastAddr.sin6_addr); // Link-local all nodes

    std::string beaconMessage = "TRGGO_PEER:" + std::to_string(g_myTcpPort) + ":" + g_myName + ":" + g_myHostName;

    while (g_running) {
        if (!g_inChat) {
            sendto(udpSocket, beaconMessage.c_str(), (int)beaconMessage.length(), 0,
                   (sockaddr*)&multicastAddr, sizeof(multicastAddr));
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    closesocket(udpSocket);
}

// Thread to listen for UDP multicast beacons from other peers
void UDPBeaconListener() {
    SOCKET udpSocket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        return;
    }

    BOOL reuse = TRUE;
    setsockopt(udpSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in6 bindAddr = {};
    bindAddr.sin6_family = AF_INET6;
    bindAddr.sin6_port = htons(DISCOVERY_PORT);
    bindAddr.sin6_addr = in6addr_any;

    if (bind(udpSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(udpSocket);
        return;
    }

    // Join multicast group ff02::1
    ipv6_mreq mreq = {};
    inet_pton(AF_INET6, "ff02::1", &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = 0; // OS chooses default interface

    if (setsockopt(udpSocket, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) == SOCKET_ERROR) {
        closesocket(udpSocket);
        return;
    }

    char recvBuf[512];
    while (g_running) {
        sockaddr_in6 senderAddr = {};
        int senderAddrLen = sizeof(senderAddr);
        int bytes = recvfrom(udpSocket, recvBuf, sizeof(recvBuf) - 1, 0, (sockaddr*)&senderAddr, &senderAddrLen);
        if (bytes > 0) {
            recvBuf[bytes] = '\0';
            std::string msg(recvBuf);

            if (msg.rfind("TRGGO_PEER:", 0) == 0) {
                size_t firstColon = msg.find(':', 11);
                if (firstColon != std::string::npos) {
                    int peerTcpPort = std::stoi(msg.substr(11, firstColon - 11));
                    
                    size_t secondColon = msg.find(':', firstColon + 1);
                    std::string peerName;
                    std::string peerHostName = "trgGo-Node";
                    
                    if (secondColon != std::string::npos) {
                        peerName = msg.substr(firstColon + 1, secondColon - (firstColon + 1));
                        peerHostName = msg.substr(secondColon + 1);
                    } else {
                        peerName = msg.substr(firstColon + 1);
                    }

                    // Skip self
                    if (peerName == g_myName && peerTcpPort == g_myTcpPort) {
                        continue;
                    }

                    char ipStr[INET6_ADDRSTRLEN];
                    inet_ntop(AF_INET6, &senderAddr.sin6_addr, ipStr, INET6_ADDRSTRLEN);
                    std::string peerIp(ipStr);

                    // Handle local loopback translation if necessary
                    if (peerIp == "::") {
                        peerIp = "::1";
                    }

                    std::string key = peerIp + ":" + std::to_string(peerTcpPort);

                    bool isNew = false;
                    {
                        std::lock_guard<std::mutex> lock(g_peerMutex);
                        if (g_peers.find(key) == g_peers.end()) {
                            isNew = true;
                        }
                        Peer p;
                        p.ip = peerIp;
                        p.port = peerTcpPort;
                        p.name = peerName;
                        p.hostname = peerHostName;
                        p.status = "online";
                        p.lastSeen = std::chrono::steady_clock::now();
                        g_peers[key] = p;
                    }

                    if (isNew) {
                        g_needRedraw = true;
                    }
                }
            }
        }
    }

    closesocket(udpSocket);
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
    serverAddr.sin6_port = 0; // Bind to port 0 to let OS select an ephemeral port

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(listenSocket);
        return;
    }

    // Retrieve the port assigned by the OS
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
                // Already in a chat, reject the incoming connection
                closesocket(clientSocket);
            } else {
                g_chatPartnerName = "Discovered Peer";
                
                // Identify peer name from our discovery list if we have it
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
            std::cout << "\r[" << g_chatPartnerName << "]: " << buffer << "\n[You]: " << std::flush;
        } else if (bytesReceived == 0) {
            std::cout << "\n[System]: Connection closed by peer.\nPress Enter to return to menu...\n";
            g_inChat = false;
            break;
        } else {
            int error = WSAGetLastError();
            if (error != WSAEINTR && error != WSAENOTSOCK) {
                std::cout << "\n[System]: Connection lost (Error: " << error << ").\nPress Enter to return to menu...\n";
            }
            g_inChat = false;
            break;
        }
    }
}

int main() {
    // 1. Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "WSAStartup failed with error: " << result << "\n";
        return 1;
    }

    // Enable ANSI escape sequence processing in Windows Console
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }

    // Retrieve local hostname
    char hostNameBuf[256];
    g_myHostName = "trgGo-Node";
    if (gethostname(hostNameBuf, sizeof(hostNameBuf)) == 0) {
        g_myHostName = hostNameBuf;
    }

    std::system("cls");
    std::cout << "===========================================\n";
    std::cout << "   trgGo - IPv6 P2P Chat with Discovery    \n";
    std::cout << "===========================================\n";
    std::cout << "Enter your Nickname: ";
    std::getline(std::cin, g_myName);
    if (g_myName.empty()) {
        g_myName = "AnonymousPeer";
    }

    // 2. Start TCP Chat Server (assigns ephemeral port)
    std::thread tcpServerThread(TCPLServer);
    // Wait briefly to make sure port is assigned
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 3. Start UDP Multicast Beacon Sender & Listener
    std::thread beaconSenderThread(UDPBeaconSender);
    std::thread beaconListenerThread(UDPBeaconListener);
    std::thread pruningThread(PruneOfflinePeers);

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

    while (g_running) {
        // Instant check if someone connected to us
        if (g_inChat && g_chatSocket != INVALID_SOCKET) {
            goto enter_chat_room;
        }

        if (g_needRedraw) {
            g_needRedraw = false;
            std::system("cls");

            std::cout << "==================================================================================\n";
            std::cout << " " << color::CYAN << color::BOLD << "trgGo P2P Console" << color::RESET 
                      << " | User: " << color::GREEN << g_myName << color::RESET 
                      << " | Host: " << g_myHostName 
                      << " | Port: " << g_myTcpPort << "\n";
            std::cout << "==================================================================================\n";

            // Print table header using user-supplied style
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
            std::cout << "----------------------------------------------------------------------------------\n";
            std::cout << "> " << currentInput << std::flush;
        }

        // Non-blocking keyboard check using conio
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 224 || ch == 0) {
                // Extended character (ignore arrow keys)
                _getch();
                continue;
            }

            if (ch == 8) { // Backspace
                if (!currentInput.empty()) {
                    currentInput.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
            } else if (ch == 13) { // Enter
                std::cout << "\n";
                std::string cmd = currentInput;
                currentInput = "";

                if (cmd == "/exit") {
                    g_running = false;
                    break;
                }

                // Parse input command formats
                std::string targetIdStr = "";
                if (cmd.rfind("engage ", 0) == 0 && cmd.length() > 7) {
                    targetIdStr = cmd.substr(7);
                } else if (cmd.rfind("initiate ", 0) == 0 && cmd.length() > 9) {
                    targetIdStr = cmd.substr(9);
                } else if (cmd.rfind("use ", 0) == 0 && cmd.length() > 4) {
                    targetIdStr = cmd.substr(4);
                } else {
                    targetIdStr = cmd; // direct number
                }

                // Clean spaces
                while (!targetIdStr.empty() && targetIdStr.front() == ' ') targetIdStr.erase(0, 1);
                while (!targetIdStr.empty() && targetIdStr.back() == ' ') targetIdStr.pop_back();

                if (!targetIdStr.empty()) {
                    try {
                        int selection = std::stoi(targetIdStr);
                        if (selection >= 1 && selection <= (int)activePeersList.size()) {
                            Peer target = activePeersList[selection - 1];
                            std::cout << "[System]: Connecting to " << target.name << " at [" << target.ip << "]:" << target.port << "...\n";

                            SOCKET connectSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
                            if (connectSocket != INVALID_SOCKET) {
                                sockaddr_in6 peerAddr = {};
                                peerAddr.sin6_family = AF_INET6;
                                peerAddr.sin6_port = htons(target.port);
                                inet_pton(AF_INET6, target.ip.c_str(), &peerAddr.sin6_addr);

                                if (connect(connectSocket, (sockaddr*)&peerAddr, sizeof(peerAddr)) != SOCKET_ERROR) {
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
            } else if (ch >= 32 && ch <= 126) { // ASCII printable
                currentInput += (char)ch;
                std::cout << (char)ch << std::flush;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

    enter_chat_room:
        if (g_inChat && g_chatSocket != INVALID_SOCKET) {
            std::system("cls");

            std::cout << "===========================================\n";
            std::cout << " Chatting with: " << g_chatPartnerName << "\n";
            std::cout << " Type /exit to close the session.\n";
            std::cout << "===========================================\n\n";

            std::thread recvThread(ReceiveMessages, g_chatSocket.load());

            while (g_inChat && g_running) {
                std::cout << "[You]: " << std::flush;
                std::string chatInput;
                std::getline(std::cin, chatInput);

                if (chatInput == "/exit") {
                    g_inChat = false;
                    break;
                }

                if (!chatInput.empty() && g_inChat) {
                    int bytesSent = send(g_chatSocket, chatInput.c_str(), (int)chatInput.length(), 0);
                    if (bytesSent == SOCKET_ERROR) {
                        std::cerr << "\n[System]: Send failed. Error: " << WSAGetLastError() << "\n";
                        g_inChat = false;
                        break;
                    }
                }
            }

            // Chat session cleanup
            shutdown(g_chatSocket, SD_BOTH);
            closesocket(g_chatSocket);
            g_chatSocket = INVALID_SOCKET;
            g_inChat = false;

            if (recvThread.joinable()) {
                recvThread.join();
            }

            std::cout << "\n[System]: Returning to online peer list...\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            g_needRedraw = true;
        }
    }

    // Full cleanup
    g_running = false;
    
    SOCKET cs = g_chatSocket.exchange(INVALID_SOCKET);
    if (cs != INVALID_SOCKET) {
        closesocket(cs);
    }

    if (tcpServerThread.joinable()) tcpServerThread.join();
    if (beaconSenderThread.joinable()) beaconSenderThread.join();
    if (beaconListenerThread.joinable()) beaconListenerThread.join();
    if (pruningThread.joinable()) pruningThread.join();

    WSACleanup();
    std::cout << "[System]: Closed. Goodbye!\n";
    return 0;
}
