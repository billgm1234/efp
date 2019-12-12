//
// Created by Anders Cedronius on 2019-12-05.
//

#ifndef EFP_UNITTEST16_H
#define EFP_UNITTEST16_H

#include "../EdgewareFrameProtocol.h"
#include <random>

#define MTU 1456 //SRT-max

class UnitTest16 {
public:
    bool startUnitTest();
private:

    std::mutex debugPrintMutex;

    void sendData(const std::vector<uint8_t> &subPacket);
    void gotData(EdgewareFrameProtocol::framePtr &packet, EdgewareFrameContent content, bool broken, uint64_t pts, uint32_t code, uint8_t stream, uint8_t flags);
    bool waitForCompletion();
    EdgewareFrameProtocol *myEFPReciever = nullptr;
    EdgewareFrameProtocol *myEFPPacker = nullptr;
    std::atomic_bool unitTestActive;
    std::atomic_bool unitTestFailed;
    int activeUnitTest = 16;

    uint64_t unitTestPacketNumberReciever = 0;
    uint64_t expectedPTS;
    uint64_t brokenCounter = 0;
    std::vector<std::vector<uint8_t>> reorderBuffer;
    std::default_random_engine randEng;
    struct TestProps {
        size_t sizeOfData = 0;
        uint64_t pts;
        bool reorder = false;
        bool loss = false;
        bool broken = false;
    };
    uint64_t counter293=0;
    std::vector<TestProps>testData;
};

#endif //EFP_UNITTEST15_H
