#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <cstdlib>
#include <cerrno>
#include "crc32.h"

struct PacketHeader {
    unsigned int type;
    unsigned int seqNum;
    unsigned int length;
    unsigned int checksum;
};

enum PacketType { START = 0, END = 1, DATA = 2, ACK = 3 };

class wSender {
private:
    int sockfd;
    struct sockaddr_in receiverAddr;
    std::string inputFile;
    std::ofstream logFile;
    unsigned int windowSize;
    std::vector<std::vector<char>> dataChunks;
    unsigned int startSeqNum;
    unsigned int base;
    unsigned int nextSeqNum;
    bool startAcked;
    bool endAcked;

    void sendStart() {
        PacketHeader startHeader;
        startHeader.type = START;
        startHeader.seqNum = htonl(startSeqNum);
        startHeader.length = 0;
        startHeader.checksum = 0;

        sendPacket(&startHeader, nullptr);
        log(startHeader);
    }

    void sendEnd() {
        PacketHeader endHeader;
        endHeader.type = END;
        endHeader.seqNum = htonl(startSeqNum);
        endHeader.length = 0;
        endHeader.checksum = 0;

        sendPacket(&endHeader, nullptr);
        log(endHeader);
    }

    void sendPacket(const PacketHeader* header, const char* data) {
        char buffer[sizeof(PacketHeader) + 1456];
        memcpy(buffer, header, sizeof(PacketHeader));
        if (data != nullptr) {
            memcpy(buffer + sizeof(PacketHeader), data, ntohl(header->length));
        }
        sendto(sockfd, buffer, sizeof(PacketHeader) + ntohl(header->length), 0,
               (struct sockaddr*)&receiverAddr, sizeof(receiverAddr));
    }

    void log(const PacketHeader& header) {
        logFile << header.type << " " << ntohl(header.seqNum) << " " << ntohl(header.length) << " " << ntohl(header.checksum) << std::endl;
    }

    bool waitForAck(unsigned int expectedSeq) {
        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);

        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;

        while (true) {
            int ret = select(sockfd + 1, &readfds, nullptr, nullptr, &timeout);
            if (ret == 0) {
                return false;
            } else if (ret == -1) {
                perror("select");
                exit(1);
            } else {
                PacketHeader ackHeader;
                socklen_t addrLen = sizeof(receiverAddr);
                ssize_t bytes = recvfrom(sockfd, &ackHeader, sizeof(ackHeader), 0,
                                         (struct sockaddr*)&receiverAddr, &addrLen);
                if (bytes < 0) {
                    perror("recvfrom");
                    exit(1);
                }
                if (ntohl(ackHeader.type) == ACK && ntohl(ackHeader.seqNum) == expectedSeq) {
                    log(ackHeader);
                    return true;
                }
            }
        }
    }

public:
    wSender(const std::string& receiverIP, int receiverPort, unsigned int windowSize,
            const std::string& inputFile, const std::string& logFile)
            : windowSize(windowSize), inputFile(inputFile), startAcked(false), endAcked(false) {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("socket");
            exit(1);
        }

        memset(&receiverAddr, 0, sizeof(receiverAddr));
        receiverAddr.sin_family = AF_INET;
        receiverAddr.sin_port = htons(receiverPort);
        inet_pton(AF_INET, receiverIP.c_str(), &receiverAddr.sin_addr);

        this->logFile.open(logFile);
        if (!this->logFile) {
            std::cerr << "Failed to open log file" << std::endl;
            exit(1);
        }

        startSeqNum = rand();
        base = 0;
        nextSeqNum = 0;

        std::ifstream file(inputFile, std::ios::binary);
        if (!file) {
            std::cerr << "Failed to open input file" << std::endl;
            exit(1);
        }

        char buffer[1456];
        while (file.read(buffer, sizeof(buffer))) {
            dataChunks.push_back(std::vector<char>(buffer, buffer + file.gcount()));
        }
        if (file.gcount() > 0) {
            dataChunks.push_back(std::vector<char>(buffer, buffer + file.gcount()));
        }
        file.close();
    }

    void run() {
        sendStart();
        if (!waitForAck(startSeqNum)) {
            std::cerr << "Failed to establish connection" << std::endl;
            exit(1);
        }

        struct timeval startTime;
        gettimeofday(&startTime, nullptr);

        while (base < dataChunks.size()) {
            while (nextSeqNum < base + windowSize && nextSeqNum < dataChunks.size()) {
                PacketHeader dataHeader;
                dataHeader.type = DATA;
                dataHeader.seqNum = htonl(nextSeqNum);
                dataHeader.length = htonl(dataChunks[nextSeqNum].size());
                dataHeader.checksum = crc32(dataChunks[nextSeqNum].data(), dataChunks[nextSeqNum].size());

                sendPacket(&dataHeader, dataChunks[nextSeqNum].data());
                log(dataHeader);
                nextSeqNum++;
            }

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sockfd, &readfds);

            struct timeval currentTime;
            gettimeofday(&currentTime, nullptr);
            long elapsed = (currentTime.tv_sec - startTime.tv_sec) * 1000000 + (currentTime.tv_usec - startTime.tv_usec);
            if (elapsed >= 500000) {
                for (unsigned int i = base; i < nextSeqNum; ++i) {
                    PacketHeader dataHeader;
                    dataHeader.type = DATA;
                    dataHeader.seqNum = htonl(i);
                    dataHeader.length = htonl(dataChunks[i].size());
                    dataHeader.checksum = crc32(dataChunks[i].data(), dataChunks[i].size());
                    sendPacket(&dataHeader, dataChunks[i].data());
                    log(dataHeader);
                }
                gettimeofday(&startTime, nullptr);
            } else {
                struct timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = 500000 - elapsed;
                select(sockfd + 1, &readfds, nullptr, nullptr, &timeout);
            }

            if (FD_ISSET(sockfd, &readfds)) {
                PacketHeader ackHeader;
                socklen_t addrLen = sizeof(receiverAddr);
                ssize_t bytes = recvfrom(sockfd, &ackHeader, sizeof(ackHeader), 0,
                                         (struct sockaddr*)&receiverAddr, &addrLen);
                if (bytes >= sizeof(ackHeader) && ntohl(ackHeader.type) == ACK) {
                    unsigned int ackSeq = ntohl(ackHeader.seqNum);
                    if (ackSeq > base) {
                        base = ackSeq;
                        gettimeofday(&startTime, nullptr);
                    }
                }
            }
        }

        sendEnd();
        while (!waitForAck(startSeqNum)) {
            sendEnd();
        }
    }

    ~wSender() {
        close(sockfd);
        logFile.close();
    }
};

int main(int argc, char* argv[]) {
    if (argc != 6) {
        std::cerr << "Usage: ./wSender <receiver-IP> <receiver-port> <window-size> <input-file> <log>" << std::endl;
        return 1;
    }

    srand(time(nullptr));

    try {
        wSender sender(argv[1], std::stoi(argv[2]), std::stoul(argv[3]), argv[4], argv[5]);
        sender.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return 0;
}