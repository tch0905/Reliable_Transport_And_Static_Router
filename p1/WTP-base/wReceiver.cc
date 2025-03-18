#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include <sys/stat.h>
#include <filesystem>
#include "crc32.h"
#include "PacketHeader.h"

#define ISDEBUG false

using PacketType::START;
using PacketType::END;
using PacketType::DATA;
using PacketType::ACK;

class wReceiver {
private:
    int sockfd;
    unsigned int windowSize;
    std::string outputDir;
    std::ofstream logFile;
    std::map<unsigned int, std::vector<char> > receivedPackets;
    unsigned int expectedSeq;
    int fileCounter;
    std::ofstream currentFile;

    void log(const PacketHeader& header) {
        logFile << to_string(header) << std::endl;
    }


    void sendAck(unsigned int seqNum) {
        PacketHeader ackHeader;
        ackHeader.type = ACK;
        ackHeader.seqNum = htonl(seqNum);
        ackHeader.length = 0;
        ackHeader.checksum = 0;

        sendPacket(&ackHeader, nullptr);
        log(ackHeader);
    }

    void sendPacket(const PacketHeader* header, const char* data) {
        char buffer[sizeof(PacketHeader) + 1456];
        memcpy(buffer, header, sizeof(PacketHeader));
        if (data != nullptr) {
            memcpy(buffer + sizeof(PacketHeader), data, ntohl(header->length));
        }
        sendto(sockfd, buffer, sizeof(PacketHeader) + ntohl(header->length), 0,
               (struct sockaddr*)&clientAddr, sizeof(clientAddr));
    }

    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen;

public:
    wReceiver(int port, unsigned int windowSize, const std::string& outputDir, const std::string& logFile)
            : windowSize(windowSize), outputDir(outputDir), expectedSeq(0), fileCounter(0) {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("socket");
            exit(1);
        }

        struct sockaddr_in serverAddr;
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        serverAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            perror("bind");
            exit(1);
        }

        this->logFile.open(logFile);
        if (!this->logFile) {
            std::cerr << "Failed to open log file" << std::endl;
            exit(1);
        }

        mkdir(outputDir.c_str(), 0777);
        try {
            // Create a directory
            if (std::filesystem::create_directory(outputDir)) {
                    std::cout << "Directory created successfully: " << outputDir << std::endl;
            } else {
                    std::cout << "Directory already exists: " << outputDir << std::endl;
            }
            } catch (const std::filesystem::filesystem_error& e) {
                std::cerr << "Error creating directory: " << e.what() << std::endl;
        }
    }

    void run() {
        char buffer[1472];
        PacketHeader header;

        clientAddrLen = sizeof(clientAddr);

        while (true) {
            ssize_t bytes = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                                     (struct sockaddr*)&clientAddr, &clientAddrLen);

            std::cout << std::endl;
            std::cout << std::endl;
            std::cout << "Receiving" << std::endl;
            if (bytes < sizeof(PacketHeader)) {
                continue;
            }

            memcpy(&header, buffer, sizeof(PacketHeader));
            header.type = header.type;
            header.seqNum = ntohl(header.seqNum);
            header.length = ntohl(header.length);
            header.checksum = ntohl(header.checksum);

            log(header);

            std::cout << to_string(header)<< std::endl;
            if (header.type == START) {
                std::cout << "START received" << std::endl;
                if (currentFile.is_open()) {
                    std::cerr << "The prev request, is not ending, expecting recevied END" << std::endl;
                    continue; // Ignore START during transfer
                }
                std::string filename = outputDir + "/FILE-" + std::to_string(fileCounter++) + ".out";
                std::cout << filename <<"<- writing data to here" << std::endl;
                currentFile.open(filename, std::ios::binary);
                if (!currentFile) {
                    std::cerr << "Failed to open output file" << std::endl;
                    continue;
                }

                // re-init
                expectedSeq = 0;
                receivedPackets.clear();

                std::cout << "Ack sent" << std::endl;
                sendAck(header.seqNum);
            }
            // DATA
            else if (header.type == DATA) {
                std::cout << "DATA received" << std::endl;
                if (!currentFile.is_open()) {
                    std::cerr << "The current file is not open" << std::endl;
                    continue; // Ignore DATA without START
                }

                char* data = buffer + sizeof(PacketHeader);
                if (header.length > 0) {
                    unsigned int computedChecksum = crc32(data, header.length);
                    if (computedChecksum != header.checksum) {
                        std::cerr << "The Checksum is diff" << std::endl;
                        // TODO: remove the command
//                        continue; // Drop corrupted packet
                    }
                }

                // Write to File
                if (header.seqNum >= expectedSeq && header.seqNum < expectedSeq + windowSize) {
                    receivedPackets[header.seqNum] = std::vector<char>(data, data + header.length);
                }

                while (receivedPackets.find(expectedSeq) != receivedPackets.end()) {
                    currentFile.write(receivedPackets[expectedSeq].data(), receivedPackets[expectedSeq].size());
                    receivedPackets.erase(expectedSeq);
                    expectedSeq++;
                }
                std::cout << "Sending ACK" << std::endl;
                sendAck(expectedSeq);
            } else if (header.type == END) {
                std::cout << "The header is END" << std::endl;
                if (currentFile.is_open()) {
                    currentFile.close();
                    sendAck(header.seqNum);
                }
            }
        }
    }

    ~wReceiver() {
        close(sockfd);
        logFile.close();
    }
};

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: ./wReceiver <port-num> <window-size> <output-dir> <log>" << std::endl;
        return 1;
    }

    try {
        wReceiver receiver(std::stoi(argv[1]), std::stoul(argv[2]), argv[3], argv[4]);
        receiver.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return 0;
}