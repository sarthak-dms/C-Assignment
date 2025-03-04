#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600 // Ensure InetPton is available
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <set>

// Pragma to link Winsock library
#pragma comment(lib, "ws2_32.lib")

// Custom JSON-like class
class SimpleJson
{
public:
    enum class Type
    {
        Object,
        Array,
        String,
        Number
    };

    SimpleJson() : m_type(Type::Object) {}
    SimpleJson(Type type) : m_type(type) {}

    static SimpleJson array()
    {
        return SimpleJson(Type::Array);
    }

    void push_back(const SimpleJson &value)
    {
        if (m_type != Type::Array)
        {
            throw std::runtime_error("Not an array");
        }
        m_array.push_back(value);
    }

    void add(const std::string &key, const SimpleJson &value)
    {
        if (m_type != Type::Object)
        {
            throw std::runtime_error("Not an object");
        }
        m_object[key] = value;
    }

    std::string dump(int indent = 4) const
    {
        std::stringstream ss;
        dump_internal(ss, 0, indent);
        return ss.str();
    }

private:
    void dump_internal(std::stringstream &ss, int current_indent, int indent_size) const
    {
        std::string indent_str(current_indent, ' ');

        switch (m_type)
        {
        case Type::Object:
        {
            ss << "{\n";
            bool first = true;
            for (const auto &pair : m_object)
            {
                if (!first)
                    ss << ",\n";
                ss << indent_str << std::string(indent_size, ' ')
                   << "\"" << pair.first << "\": ";
                pair.second.dump_internal(ss, current_indent + indent_size, indent_size);
                first = false;
            }
            ss << "\n"
               << indent_str << "}";
            break;
        }
        case Type::Array:
        {
            ss << "[";
            bool first = true;
            for (const auto &item : m_array)
            {
                if (!first)
                    ss << ", ";
                item.dump_internal(ss, current_indent + indent_size, indent_size);
                first = false;
            }
            ss << "]";
            break;
        }
        case Type::String:
        {
            ss << "\"" << m_string << "\"";
            break;
        }
        case Type::Number:
        {
            ss << m_number;
            break;
        }
        }
    }

public:
    SimpleJson(const std::string &value) : m_type(Type::String), m_string(value) {}
    SimpleJson(int value) : m_type(Type::Number), m_number(std::to_string(value)) {}

private:
    Type m_type;
    std::map<std::string, SimpleJson> m_object;
    std::vector<SimpleJson> m_array;
    std::string m_string;
    std::string m_number;
};

class ABXExchangeClient
{
public:
    ABXExchangeClient(const std::string &host, int port = 3000)
        : m_host(host), m_port(port)
    {
        // Initialize Winsock
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~ABXExchangeClient()
    {
        WSACleanup();
    }

    struct Packet
    {
        std::string symbol;
        char type;
        int32_t quantity;
        int32_t price;
        int32_t sequence;

        SimpleJson toJson() const
        {
            SimpleJson json(SimpleJson::Type::Object);
            json.add("symbol", symbol);
            json.add("type", std::string(1, type));
            json.add("quantity", quantity);
            json.add("price", price);
            json.add("sequence", sequence);
            return json;
        }
    };

    void run()
    {
        std::cout << "Call to Stream All Packets" << std::endl;
        streamAllPackets();
        std::cout << "Call Request Missing Sequence" << std::endl;
        requestMissingSequences();
        std::cout << "Call Output Json File" << std::endl;
        outputToJsonFile();
    }

private:
    std::string m_host;
    int m_port;
    std::map<int32_t, Packet> m_receivedPackets;
    int32_t m_maxSequence = -1;

    SOCKET createSocket()
    {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
        {
            throw std::runtime_error("Socket creation failed");
        }
        return sock;
    }

    void connectSocket(SOCKET sock)
    {
        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(m_port);

        // Alternative: Use inet_addr instead of InetPton
        serverAddr.sin_addr.s_addr = inet_addr(m_host.c_str());
        if (serverAddr.sin_addr.s_addr == INADDR_NONE)
        {
            closesocket(sock);
            throw std::runtime_error("Invalid address");
        }

        if (connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
        {
            closesocket(sock);
            throw std::runtime_error("Connection failed: " + std::to_string(WSAGetLastError()));
        }
    }

    void streamAllPackets()
    {
        SOCKET sock = createSocket();
        try
        {
            connectSocket(sock);
            uint8_t request[2] = {1, 0};
            send(sock, reinterpret_cast<const char *>(request), sizeof(request), 0);
            receivePackets(sock);
        }
        catch (...)
        {
            closesocket(sock);
            throw;
        }
        closesocket(sock);
    }

    void receivePackets(SOCKET sock)
    {
        const int PACKET_SIZE = 22;
        char buffer[PACKET_SIZE];
        int totalBytesRead = 0;

        while (true)
        {
            int bytesRead = recv(sock, buffer + totalBytesRead, PACKET_SIZE - totalBytesRead, 0);

            if (bytesRead == 0)
            {
                std::cout << "[INFO] Server closed the connection." << std::endl;
                break; // Server closed connection
            }

            if (bytesRead == SOCKET_ERROR)
            {
                std::cerr << "[ERROR] Receive failed: " << WSAGetLastError() << std::endl;
                return;
            }

            totalBytesRead += bytesRead;

            if (totalBytesRead < PACKET_SIZE)
            {
                std::cout << "[WARNING] Partial packet received (" << totalBytesRead << " bytes), waiting for more..." << std::endl;
                continue; // Wait for the remaining bytes
            }

            // Full packet received, process it
            Packet packet = parsePacket(buffer);
            std::cout << "[DEBUG] Packet Received: " << packet.symbol
                      << ", Type: " << packet.type
                      << ", Qty: " << packet.quantity
                      << ", Price: " << packet.price
                      << ", Seq: " << packet.sequence << std::endl;

            // Store packet in map
            m_receivedPackets[packet.sequence] = packet;
            m_maxSequence = std::max(m_maxSequence, packet.sequence);

            // Reset buffer for the next packet
            totalBytesRead = 0;
        }
    }

    Packet parsePacket(const char *buffer)
    {
        Packet packet;
        char symbolBuffer[5] = {0};
        std::memcpy(symbolBuffer, buffer, 4);
        packet.symbol = symbolBuffer;
        packet.type = buffer[4];
        packet.quantity = ntohl(*reinterpret_cast<const uint32_t *>(buffer + 5));
        packet.price = ntohl(*reinterpret_cast<const uint32_t *>(buffer + 9));
        packet.sequence = ntohl(*reinterpret_cast<const uint32_t *>(buffer + 13));
        return packet;
    }

    void requestMissingSequences()
    {
        std::cout << "[requestMissingSequences] maxSequence: " << m_maxSequence << std::endl;

        SOCKET sock = createSocket(); // Create a single socket
        try
        {
            connectSocket(sock); // Connect once

            for (int32_t i = 0; i <= m_maxSequence; ++i)
            {
                if (m_receivedPackets.find(i) == m_receivedPackets.end())
                {
                    // Prepare Type 2 request: 2 (Type) + 1 Byte (Sequence Number)
                    uint8_t request[2];
                    request[0] = 2;                              // Type 2: Resend Packet
                    request[1] = static_cast<uint8_t>(i & 0xFF); // Ensure only 1 byte (LSB) is sent

                    std::cout << "[INFO] Requesting missing sequence: " << static_cast<int>(request[1]) << std::endl;

                    int sentBytes = send(sock, reinterpret_cast<const char *>(request), sizeof(request), 0);

                    std::cout << "here" << std::endl;
                    if (sentBytes == SOCKET_ERROR)
                    {
                        std::cerr << "[ERROR] Failed to send request for sequence " << i
                                  << ": " << WSAGetLastError() << std::endl;
                        break; // Stop on failure
                    }

                    receivePackets(sock); // Receive response
                }
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "[ERROR] Exception in requestMissingSequences: " << e.what() << std::endl;
        }

        closesocket(sock); // Close the connection after all requests
    }

    void outputToJsonFile()
    {
        SimpleJson jsonOutput = SimpleJson::array();
        for (int32_t i = 0; i <= 100000; ++i)
        {
            if (m_receivedPackets.find(i) != m_receivedPackets.end())
            {
                jsonOutput.push_back(m_receivedPackets[i].toJson());
            }
        }

        std::ofstream outputFile("abx_exchange_data.json");
        outputFile << jsonOutput.dump() << std::endl;
    }

public:
    static void ExecuteClient(const std::string &host)
    {
        ABXExchangeClient client(host);
        client.run();
    }
};

int main(int argc, char *argv[])
{
    std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    ABXExchangeClient::ExecuteClient(host);
    return 0;
}
