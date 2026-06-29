#ifndef UNICODE
#define UNICODE
#endif

#define WIN32_LEAN_AND_MEAN

#include <iostream>
#include <string>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>

// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

#define BUFFER_SIZE 2048

void ReceiveMessages(SOCKET peerSocket) {
    char buffer[BUFFER_SIZE];
    while (true) {
        int bytesReceived = recv(peerSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            // Clear current line and print the message
            std::cout << "\r[Peer]: " << buffer << "\n[You]: " << std::flush;
        } else if (bytesReceived == 0) {
            std::cout << "\n[System]: Connection closed by peer.\nPress Enter to exit...\n";
            break;
        } else {
            // If the socket was closed intentionally by us, just exit quietly
            int error = WSAGetLastError();
            if (error != WSAEINTR && error != WSAENOTSOCK) {
                std::cout << "\n[System]: Receive failed with error code: " << error << "\nPress Enter to exit...\n";
            }
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

    SOCKET chatSocket = INVALID_SOCKET;
    int mode = 0;

    std::cout << "===========================================\n";
    std::cout << "       trgGo - IPv6 C++ P2P Chat           \n";
    std::cout << "===========================================\n";
    std::cout << "Choose Mode:\n";
    std::cout << "  1. Listen for connection\n";
    std::cout << "  2. Connect to a peer\n";
    std::cout << "Enter choice (1 or 2): ";
    std::cin >> mode;
    std::cin.ignore(); // clear newline character from buffer

    if (mode == 1) {
        // --- LISTEN MODE ---
        int port;
        std::cout << "Enter port to listen on (e.g. 8080): ";
        std::cin >> port;
        std::cin.ignore();

        SOCKET listenSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET) {
            std::cerr << "Failed to create listen socket. Error: " << WSAGetLastError() << "\n";
            WSACleanup();
            return 1;
        }

        // Disable IPV6_V6ONLY to allow only IPv6 traffic or keep default (usually IPv6 only on modern Windows)
        // We will leave it default so it behaves strictly as IPv6.

        sockaddr_in6 serverAddr = {};
        serverAddr.sin6_family = AF_INET6;
        serverAddr.sin6_addr = in6addr_any; // bind to all available interfaces
        serverAddr.sin6_port = htons(port);

        if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "Bind failed with error: " << WSAGetLastError() << "\n";
            closesocket(listenSocket);
            WSACleanup();
            return 1;
        }

        if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
            std::cerr << "Listen failed with error: " << WSAGetLastError() << "\n";
            closesocket(listenSocket);
            WSACleanup();
            return 1;
        }

        std::cout << "[System]: Listening on port " << port << " for IPv6 connections...\n";

        sockaddr_in6 clientAddr = {};
        int clientAddrSize = sizeof(clientAddr);
        chatSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);
        if (chatSocket == INVALID_SOCKET) {
            std::cerr << "Accept failed with error: " << WSAGetLastError() << "\n";
            closesocket(listenSocket);
            WSACleanup();
            return 1;
        }

        // Output client's IP
        char clientIpStr[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &clientAddr.sin6_addr, clientIpStr, INET6_ADDRSTRLEN);
        std::cout << "[System]: Connected to peer [" << clientIpStr << "]!\n";

        closesocket(listenSocket); // No longer need the listener

    } else if (mode == 2) {
        // --- CONNECT MODE ---
        std::string ipStr;
        int port;

        std::cout << "Enter peer's IPv6 address (e.g. ::1 for localhost): ";
        std::getline(std::cin, ipStr);
        std::cout << "Enter peer's port: ";
        std::cin >> port;
        std::cin.ignore();

        chatSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (chatSocket == INVALID_SOCKET) {
            std::cerr << "Failed to create socket. Error: " << WSAGetLastError() << "\n";
            WSACleanup();
            return 1;
        }

        sockaddr_in6 clientAddr = {};
        clientAddr.sin6_family = AF_INET6;
        clientAddr.sin6_port = htons(port);
        if (inet_pton(AF_INET6, ipStr.c_str(), &clientAddr.sin6_addr) != 1) {
            std::cerr << "Invalid IPv6 address format.\n";
            closesocket(chatSocket);
            WSACleanup();
            return 1;
        }

        std::cout << "[System]: Connecting to [" << ipStr << "] on port " << port << "...\n";

        if (connect(chatSocket, (sockaddr*)&clientAddr, sizeof(clientAddr)) == SOCKET_ERROR) {
            std::cerr << "Connect failed with error: " << WSAGetLastError() << "\n";
            closesocket(chatSocket);
            WSACleanup();
            return 1;
        }

        std::cout << "[System]: Connected to peer!\n";

    } else {
        std::cout << "Invalid choice. Exiting.\n";
        WSACleanup();
        return 1;
    }

    std::cout << "\n-------------------------------------------\n";
    std::cout << " Chat session started. Type /exit to close.\n";
    std::cout << "-------------------------------------------\n\n";

    // Start background thread to receive messages
    std::thread recvThread(ReceiveMessages, chatSocket);

    // Main thread handles sending messages
    std::string input;
    while (true) {
        std::cout << "[You]: " << std::flush;
        std::getline(std::cin, input);

        if (input == "/exit") {
            break;
        }

        if (!input.empty()) {
            int bytesSent = send(chatSocket, input.c_str(), (int)input.length(), 0);
            if (bytesSent == SOCKET_ERROR) {
                std::cerr << "\n[System]: Send failed with error: " << WSAGetLastError() << "\n";
                break;
            }
        }
    }

    // Clean shutdown
    std::cout << "[System]: Closing connection...\n";
    shutdown(chatSocket, SD_BOTH);
    closesocket(chatSocket);

    // Wait for receive thread to finish
    if (recvThread.joinable()) {
        recvThread.join();
    }

    WSACleanup();
    std::cout << "[System]: Goodbye!\n";
    return 0;
}
