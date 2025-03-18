#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include <sys/stat.h>
#include "crc32.h"

struct PacketHeader {
    unsigned int type;
    unsigned int seqNum;
    unsigned int length;
    unsigned int checksum;
};

enum PacketType { START = 0, END = 1, DATA = 2, ACK = 3 };

class wReceiver {
private:
    int sockfd;
    unsigned int windowSize;
    std::string outputDir;
    std::ofstream logFile;
    std::map<unsigned int, std::vector<char>> receivedPackets;
    unsigned int expectedSeq;
    int fileCounter;
    std::ofstream currentFile;

    void log(const PacketHeader& header) {
        logFile << ntohl(header.type) << " " << ntohl(header.seqNum) << " " << ntohl(header.length) << " " << ntohl(header.checksum) << std::endl;
    }

    void sendAck(unsigned int seqNum) {
        PacketHeader ackHeader;
        ackHeader.type = ACK;
        ackHeader.seqNum = htonl(seqNum);
        ackHeader.length = 0;
        ackHeader.checksum = 0;

        sendto(sockfd, &ackHeader, sizeof(ackHeader), 0, (struct sockaddr*)&clientAddr, sizeof(clientAddr));
        log(ackHeader);
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
    }

    void run() {
        char buffer[1472];
        PacketHeader header;

        clientAddrLen = sizeof(clientAddr);

        while (true) {
            ssize_t bytes = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                                     (struct sockaddr*)&clientAddr, &clientAddrLen);
            if (bytes < sizeof(PacketHeader)) {
                continue;
            }

            memcpy(&header, buffer, sizeof(PacketHeader));
            header.type = ntohl(header.type);
            header.seqNum = ntohl(header.seqNum);
            header.length = ntohl(header.length);
            header.checksum = ntohl(header.checksum);

            log(header);

            if (header.type == START) {
                if (currentFile.is_open()) {
                    continue; // Ignore START during transfer
                }
                std::string filename = outputDir + "/FILE-" + std::to_string(fileCounter++) + ".out";
                currentFile.open(filename, std::ios::binary);
                if (!currentFile) {
                    std::cerr << "Failed to open output file" << std::endl;
                    continue;
                }
                expectedSeq = 0;
                receivedPackets.clear();
                sendAck(header.seqNum);
            } else if (header.type == DATA) {
                if (!currentFile.is_open()) {
                    continue; // Ignore DATA without START
                }

                char* data = buffer + sizeof(PacketHeader);
                if (header.length > 0) {
                    unsigned int computedChecksum = crc32(data, header.length);
                    if (computedChecksum != header.checksum) {
                        continue; // Drop corrupted packet
                    }
                }

                if (header.seqNum >= expectedSeq && header.seqNum < expectedSeq + windowSize) {
                    receivedPackets[header.seqNum] = std::vector<char>(data, data + header.length);
                }

                while (receivedPackets.find(expectedSeq) != receivedPackets.end()) {
                    currentFile.write(receivedPackets[expectedSeq].data(), receivedPackets[expectedSeq].size());
                    receivedPackets.erase(expectedSeq);
                    expectedSeq++;
                }

                sendAck(expectedSeq);
            } else if (header.type == END) {
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