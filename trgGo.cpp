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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>

#pragma comment(lib, "Ws2_32.lib")

#define DISCOVERY_PORT 9999
#define BUFFER_SIZE 2048

struct Peer {
    std::string ip;
    int port;
    std::string name;
    std::chrono::steady_clock::time_point lastSeen;
};

// Global variables
std::map<std::string, Peer> g_peers;
std::mutex g_peerMutex;
std::atomic<bool> g_running(true);
std::atomic<SOCKET> g_chatSocket(INVALID_SOCKET);
std::atomic<bool> g_inChat(false);
std::string g_myName;
int g_myTcpPort = 0;

// Prune peers that haven't sent a beacon in 6 seconds
void PruneOfflinePeers() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_peerMutex);
        for (auto it = g_peers.begin(); it != g_peers.end(); ) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastSeen).count();
            if (duration > 6) {
                it = g_peers.erase(it);
            } else {
                ++it;
            }
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

    std::string beaconMessage = "TRGGO_PEER:" + std::to_string(g_myTcpPort) + ":" + g_myName;

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
                    std::string peerName = msg.substr(firstColon + 1);

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

                    std::lock_guard<std::mutex> lock(g_peerMutex);
                    Peer p;
                    p.ip = peerIp;
                    p.port = peerTcpPort;
                    p.name = peerName;
                    p.lastSeen = std::chrono::steady_clock::now();
                    g_peers[key] = p;
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
                g_chatSocket = clientSocket;
                g_inChat = true;
                std::cout << "\n[System]: Incoming connection accepted! Press ENTER to start chatting...\n";
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
            std::cout << "\r[Peer]: " << buffer << "\n[You]: " << std::flush;
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

// Renders the online peers in the console
void PrintOnlinePeers(std::vector<Peer>& activePeersList) {
    std::cout << "\n===========================================\n";
    std::cout << " Your Name: " << g_myName << " (Listening on port " << g_myTcpPort << ")\n";
    std::cout << "===========================================\n";
    std::cout << "Online Users:\n";

    std::lock_guard<std::mutex> lock(g_peerMutex);
    activePeersList.clear();
    int idx = 1;
    for (const auto& pair : g_peers) {
        activePeersList.push_back(pair.second);
        std::cout << "  [" << idx++ << "] " << pair.second.name << " (" << pair.second.ip << ":" << pair.second.port << ")\n";
    }

    if (g_peers.empty()) {
        std::cout << "  (No peers online yet. Searching...)\n";
    }

    std::cout << "-------------------------------------------\n";
    std::cout << "Type a peer number [1-" << g_peers.size() << "] to connect,\n";
    std::cout << "Type /refresh to refresh the peer list,\n";
    std::cout << "Type /exit to quit.\n";
    std::cout << "-------------------------------------------\n";
}

int main() {
    // 1. Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "WSAStartup failed with error: " << result << "\n";
        return 1;
    }

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

    while (g_running) {
        PrintOnlinePeers(activePeersList);
        std::cout << "> " << std::flush;

        std::string input;
        std::getline(std::cin, input);

        if (input == "/exit") {
            g_running = false;
            break;
        }

        if (input == "/refresh" || input.empty()) {
            // Check if we were connected to while waiting
            if (g_inChat && g_chatSocket != INVALID_SOCKET) {
                // Transition into chat room loop
                goto enter_chat_room;
            }
            continue;
        }

        // Check if we got connected to while typing
        if (g_inChat && g_chatSocket != INVALID_SOCKET) {
            goto enter_chat_room;
        }

        // Try parsing selection
        try {
            int selection = std::stoi(input);
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
                        g_chatSocket = connectSocket;
                        g_inChat = true;
                        std::cout << "[System]: Connected to peer!\n";
                    } else {
                        std::cerr << "[System]: Failed to connect to peer. Error: " << WSAGetLastError() << "\n";
                        closesocket(connectSocket);
                    }
                }
            } else {
                std::cout << "[System]: Invalid peer number.\n";
            }
        } catch (...) {
            std::cout << "[System]: Unknown command or invalid selection.\n";
        }

    enter_chat_room:
        if (g_inChat && g_chatSocket != INVALID_SOCKET) {
            std::cout << "\n-------------------------------------------\n";
            std::cout << " Chat session started. Type /exit to close.\n";
            std::cout << "-------------------------------------------\n\n";

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
                        std::cerr << "\n[System]: Send failed with error: " << WSAGetLastError() << "\n";
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

            std::cout << "\n[System]: Returned to the peer list.\n";
        }
    }

    // Full shutdown
    g_running = false;
    
    // Wake up sockets if blocked
    SOCKET cs = g_chatSocket.exchange(INVALID_SOCKET);
    if (cs != INVALID_SOCKET) {
        closesocket(cs);
    }

    // Join all background threads
    if (tcpServerThread.joinable()) tcpServerThread.join();
    if (beaconSenderThread.joinable()) beaconSenderThread.join();
    if (beaconListenerThread.joinable()) beaconListenerThread.join();
    if (pruningThread.joinable()) pruningThread.join();

    WSACleanup();
    std::cout << "[System]: Closed. Goodbye!\n";
    return 0;
}
