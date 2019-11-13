//
// Created by Anders Cedronius on 2019-11-11.
//

#include "EdgewareFrameProtocol.h"

//Constructor
EdgewareFrameProtocol::EdgewareFrameProtocol(uint32_t setMTU) {
    if (setMTU > UINT_MAX) {
        LOGGER(true, LOGG_FATAL, "MTU Larger than 65535, that is illegal.");
    } else if (setMTU < UINT8_MAX) {
        LOGGER(true, LOGG_FATAL, "MTU less than 255 is not accepted.");
    } else {
        currentMTU = setMTU;
    }
    threadActive = false;
    isThreadActive = false;
    LOGGER(true, LOGG_NOTIFY, "EdgewareFrameProtocol constructed");
}

//Destructor
EdgewareFrameProtocol::~EdgewareFrameProtocol() {
    LOGGER(true, LOGG_NOTIFY, "EdgewareFrameProtocol destruct");
}

//Dummy callback for transmitter
void EdgewareFrameProtocol::sendData(const std::vector<uint8_t> &subPacket) {
    LOGGER(true, LOGG_ERROR, "Implement the sendCallback method for the protocol to work.");
}
//Dummy callback for reciever
void EdgewareFrameProtocol::gotData(const std::vector<uint8_t> &packet, EdgewareFrameContent content, bool broken) {
    LOGGER(true, LOGG_ERROR, "Implement the recieveCallback method for the protocol to work.");
}

//This method is generating a linear uint64_t counter from the linear uint16_t
//counter. The maximum loss / hole this calculator can handle is (UINT16_MAX + 1)
uint64_t EdgewareFrameProtocol::superFrameRecalculator(uint16_t superframe) {
    if (superFrameFirstTime) {
        oldSuperframeNumber = (int64_t) superframe;
        superFrameRecalc = oldSuperframeNumber;
        superFrameFirstTime = false;
        return superFrameRecalc;
    }

    int64_t superFrameDiff = (int64_t) superframe - oldSuperframeNumber;
    oldSuperframeNumber = (int64_t) superframe;

    if (superFrameDiff > 0) {
        superFrameRecalc += superFrameDiff;
    } else {
        superFrameRecalc += ((UINT16_MAX + 1) - abs(superFrameDiff));
    }
    return superFrameRecalc;
}

//Unpack method for type1 packets. Type1 packets are parts of frames larger than the MTU
EdgewareFrameMessages EdgewareFrameProtocol::unpackType1(const std::vector<uint8_t> &subPacket) {
    std::lock_guard<std::mutex> lock(netMtx);

    EdgewareFrameType1 type1Frame = *(EdgewareFrameType1 *) subPacket.data();
    Bucket *thisBucket = &bucketList[(uint8_t) type1Frame.superFrameNo];

    //is this entry in the buffer active if no. create one else fill in the data?
    if (!thisBucket->active) {
        //LOGGER(false,LOGG_NOTIFY,"Setting: " << unsigned(type1Frame.superFrameNo));
        thisBucket->active = true;
        thisBucket->haveRecievedPacket.reset();
        thisBucket->haveRecievedPacket[type1Frame.fragmentNo] = 1;
        thisBucket->deliveryOrder = superFrameRecalculator(type1Frame.superFrameNo);
        thisBucket->dataContent = type1Frame.dataContent;
        thisBucket->timeout = bucketTimeout;
        thisBucket->fragmentCounter = 0;
        thisBucket->ofFragmentNo = type1Frame.ofFragmentNo;
        thisBucket->fragmentSize = (subPacket.size() - sizeof(EdgewareFrameType1));
        thisBucket->bucketData.clear();
        thisBucket->bucketData.insert(thisBucket->bucketData.end(), subPacket.begin() + sizeof(EdgewareFrameType1),
                                      subPacket.end());
        return EdgewareFrameMessages::noError;
    }

    //I'm getting a packet with data larger than the expected size
    //this can be generated by wraparound in the bucket bucketList
    //The notification about more than 50% buffer full level should already
    //be triggered by now.

    if (thisBucket->ofFragmentNo < type1Frame.fragmentNo) {
        LOGGER(true, LOGG_FATAL, "bufferOutOfBounds");
        thisBucket->active = false;
        return EdgewareFrameMessages::bufferOutOfBounds;
    }

    //Have I already recieved this packet before? (1+1 first come first serve or just a duplicate)
    if (thisBucket->haveRecievedPacket[type1Frame.fragmentNo] == 1) {
        return EdgewareFrameMessages::duplicatePacketRecieved;
    } else {
        thisBucket->haveRecievedPacket[type1Frame.fragmentNo] = 1;
    }

    //If for example there is one packet split up into type1 and type2 frame.
    //Then for some reason the unpacker recieves the out of order
    //Then the fragment size is unknown.. Lets fill it out now.. when we know.
    if (!thisBucket->fragmentSize) {
        thisBucket->fragmentSize = (subPacket.size() - sizeof(EdgewareFrameType1));
    }

    //Let's re-set the timout and let also add +1 to the fragment counter
    thisBucket->timeout = bucketTimeout;
    thisBucket->fragmentCounter++;

    //this is fragment data at the end of the packet fill it in and return
    if (thisBucket->fragmentCounter == type1Frame.fragmentNo) {
        thisBucket->bucketData.insert(thisBucket->bucketData.end(), subPacket.begin() + sizeof(EdgewareFrameType1),
                                      subPacket.end());
        return EdgewareFrameMessages::noError;
    }


    //This is a fragment of data at a position further away than the bucket contains.. fill at the end
    size_t insertDataPointer = thisBucket->fragmentSize * type1Frame.fragmentNo;
    if (thisBucket->bucketData.size() < insertDataPointer) {
        thisBucket->bucketData.insert(thisBucket->bucketData.end(), subPacket.begin() + sizeof(EdgewareFrameType1),
                                      subPacket.end());
        return EdgewareFrameMessages::noError;
    }

    //Or this data needs to be inserted somewhere between the start and the end
    thisBucket->bucketData.insert(thisBucket->bucketData.begin() + insertDataPointer,
                                  subPacket.begin() + sizeof(EdgewareFrameType1), subPacket.end());
    return EdgewareFrameMessages::noError;
}

// Unpack method for type2 packets. Where we know there is also type 1 packets involved.
// Type2 packets are parts of frames smaller than the MTU
// The data IS the last data of a sequence
// See the comments from above.

EdgewareFrameMessages EdgewareFrameProtocol::unpackType2LastFrame(const std::vector<uint8_t> &subPacket) {
    std::lock_guard<std::mutex> lock(netMtx);
    EdgewareFrameType2 type2Frame = *(EdgewareFrameType2 *) subPacket.data();
    Bucket *thisBucket = &bucketList[(uint8_t) type2Frame.superFrameNo];

    if (!thisBucket->active) {
        thisBucket->active = true;
        thisBucket->haveRecievedPacket.reset();
        thisBucket->haveRecievedPacket[type2Frame.fragmentNo] = 1;
        thisBucket->deliveryOrder = superFrameRecalculator(type2Frame.superFrameNo);
        thisBucket->dataContent = type2Frame.dataContent;
        thisBucket->timeout = bucketTimeout;
        thisBucket->fragmentCounter = 0;
        thisBucket->ofFragmentNo = type2Frame.ofFragmentNo;
        thisBucket->fragmentSize = 0;
        thisBucket->bucketData.clear();
        thisBucket->bucketData.insert(thisBucket->bucketData.end(), subPacket.begin() + sizeof(EdgewareFrameType2),
                                      subPacket.end());
        return EdgewareFrameMessages::noError;
    }

    if (thisBucket->ofFragmentNo < type2Frame.fragmentNo) {
        LOGGER(true, LOGG_FATAL, "bufferOutOfBounds");
        thisBucket->active = false;
        return EdgewareFrameMessages::bufferOutOfBounds;
    }

    if (thisBucket->haveRecievedPacket[type2Frame.fragmentNo] == 1) {
        return EdgewareFrameMessages::duplicatePacketRecieved;
    } else {
        thisBucket->haveRecievedPacket[type2Frame.fragmentNo] = 1;
    }

    thisBucket->timeout = bucketTimeout;
    thisBucket->fragmentCounter++;
    thisBucket->bucketData.insert(thisBucket->bucketData.end(), subPacket.begin() + sizeof(EdgewareFrameType2),
                                  subPacket.end());
    return EdgewareFrameMessages::noError;
}


// This is the thread going trough the buckets to see if they should be delivered to
// the 'user'

void EdgewareFrameProtocol::unpackerWorker(uint32_t timeout) {
    //Set the defaults. meaning the thread is running and there is no head of line blocking action going on.
    threadActive = true;
    isThreadActive = true;
    bool foundHeadOfLineBlocking = false;
    uint32_t headOfLineBlockingCounter = 0;
    uint64_t headOfLineBlockingTail = 0;

    while (threadActive) {
        usleep(1000); //Check all active buckets each milisecond
        uint32_t activeCount = 0;
        std::vector<CandidateToDeliver> vec;
        uint64_t deliveryOrderOldest = UINT64_MAX;

        //The default mode is not to clear any buckets
        bool clearHeadOfLineBuckets=false;
        //If im in head of blocking garbage collect mode.
        if (foundHeadOfLineBlocking) {
            //If some one instructed me to timeout then let's timeout first
            if (headOfLineBlockingCounter) {
                headOfLineBlockingCounter--;
            } else {
                //Timeout triggered.. Let's garbage collect
                clearHeadOfLineBuckets=true;
                foundHeadOfLineBlocking=false;
            }
        }

        netMtx.lock();

        //Scan trough all buckets
        for (int i = 0; i < UINT8_MAX; i++) {
            //Only work with the buckets that are active
            if (bucketList[i].active) {
                //Keep track of number of active buckets
                activeCount++;

                //save the number of the oldest bucket in queue to be delivered
                if (deliveryOrderOldest > bucketList[i].deliveryOrder) {
                    deliveryOrderOldest = bucketList[i].deliveryOrder;
                }

                //Are we cleaning out old buckets and did we found a head to timout?
                if ((bucketList[i].deliveryOrder < headOfLineBlockingTail) && clearHeadOfLineBuckets) {
                    bucketList[i].timeout = 1;
                }

                //If the bucket is ready to be delivered or is the bucket timedout?
                if (bucketList[i].fragmentCounter == bucketList[i].ofFragmentNo) {
                    vec.push_back(CandidateToDeliver(bucketList[i].deliveryOrder, i, false));
                } else if (!--bucketList[i].timeout) {
                    vec.push_back(CandidateToDeliver(bucketList[i].deliveryOrder, i, true));
                    bucketList[i].timeout = 1; //We want to timeout this again if head of line blocking is on
                }
            }
        }

        //Do we got any timedout buckets or finished buckets?
        if (vec.size()) {
            //Sort them in delivery order
            std::sort(vec.begin(), vec.end(), sortDeliveryOrder());

            //So ok we have cleared the head send it all out
            if (clearHeadOfLineBuckets) {
                for (auto &x: vec) {
                    recieveCallback(bucketList[x.bucket].bucketData, bucketList[x.bucket].dataContent, x.broken);
                    bucketList[x.bucket].active = false;
                }
            } else {

                //in this run we have not cleared the head.. is there a head to clear?
                //We can't be in waitning for timout and we can't have a 0 time-out
                //A 0 timout means out of order delivery else we-re here. then sleap.. then
                //spin the timouts then clear the head.

                //Check for head of line blocking only if hol-timoeut is set
                if (deliveryOrderOldest < bucketList[vec[0].bucket].deliveryOrder && headOfLineBlockingTimeout &&
                    !foundHeadOfLineBlocking) {
                    LOGGER(false, LOGG_NOTIFY, "HOL found"); //FIXME-REMOVE
                    foundHeadOfLineBlocking = true; //Found hol
                    headOfLineBlockingCounter = headOfLineBlockingTimeout; //Number of ms to spin
                    headOfLineBlockingTail = bucketList[vec[0].bucket].deliveryOrder; //This is the tail
                }

                //Deliver only when head of line blocking is cleared and were hopfully back to normal
                if (!foundHeadOfLineBlocking) {
                    for (auto &x: vec) {
                        recieveCallback(bucketList[x.bucket].bucketData, bucketList[x.bucket].dataContent, x.broken);
                        bucketList[x.bucket].active = false;
                    }
                }
            }
        }
        netMtx.unlock();

        //Is more than 50% of the buffer used... Keep track of this in bad network conditions.
        if (activeCount > UINT8_MAX / 2) {
            LOGGER(true, LOGG_FATAL, "Current active buckets are more than half the circular buffer.");
        }

    }
    isThreadActive = false;
}

//Start reciever worker thread
void EdgewareFrameProtocol::startUnpacker(uint32_t bucketTimeoutMaster, uint32_t holTimeoutMaster) {
    if (bucketTimeoutMaster == 0) {
        LOGGER(true, LOGG_FATAL, "bucketTimeoutMaster cant be 0 forcing 1");
        bucketTimeoutMaster = 1;
    }
    bucketTimeout = bucketTimeoutMaster;
    headOfLineBlockingTimeout = holTimeoutMaster;
    std::thread(std::bind(&EdgewareFrameProtocol::unpackerWorker, this, bucketTimeoutMaster)).detach();
}

//Stop reciever worker thread
void EdgewareFrameProtocol::stopUnpacker() {
    threadActive = false;
    uint32_t lockProtect = 1000;
    while (isThreadActive) {
        usleep(1000);
        if (!--lockProtect) {
            LOGGER(true, LOGG_FATAL, "Thread not stopping fatal.");
            break;
        }
    }
}

//Unpack method. We recieved a fragment of data or a full frame. Lets unpack it
EdgewareFrameMessages EdgewareFrameProtocol::unpack(const std::vector<uint8_t> &subPacket) {
    //Type 0 packet. Discard and continue
    //Type 0 packets can be used to fill with user data outside efp packets
    //Type 1 are packets larger than MTU
    //Type 2 are packets smaller than MTU
    //Type 2 packets are also used at the end of TYPE1 packet superFrames
    //They have type2Frame.ofFragmentNo larger than 0

    if (subPacket[0] == Frametype::type0) {
        return EdgewareFrameMessages::noError;
    } else if (subPacket[0] == Frametype::type1) {
        return unpackType1(subPacket);
    } else if (subPacket[0] == Frametype::type2) {
        if (subPacket.size() < sizeof(EdgewareFrameType2)) {
            return EdgewareFrameMessages::framesizeMismatch;
        }
        EdgewareFrameType2 type2Frame = *(EdgewareFrameType2 *) subPacket.data();
        if (type2Frame.ofFragmentNo > 0) {
            if (type2Frame.ofFragmentNo == type2Frame.fragmentNo) {
                return unpackType2LastFrame(subPacket);
            } else {
                return EdgewareFrameMessages::endOfPacketError;
            }
        }

        //This is a single frame smaller than MTU
        //Simplest case
        //FIXME - multi delivery same source is not handeled

        recieveCallback(std::vector<uint8_t>(subPacket.begin() + sizeof(EdgewareFrameType2), subPacket.end()),
                        type2Frame.dataContent, false);
    } else {
        return EdgewareFrameMessages::unknownFrametype;
    }
    return EdgewareFrameMessages::noError;
}

//Pack data method. Fragments the data and calls the sendPAcket method at the host level.
EdgewareFrameMessages
EdgewareFrameProtocol::packAndSend(const std::vector<uint8_t> &packet, EdgewareFrameContent dataContent) {
    if (packet.size() > currentMTU * UINT_MAX) {
        return EdgewareFrameMessages::tooLargeFrame;
    }

    if ((packet.size() + sizeof(EdgewareFrameType2)) <= currentMTU) {
        EdgewareFrameType2 type2Frame;
        type2Frame.superFrameNo = superFrameNo;
        type2Frame.dataContent = dataContent;
        type2Frame.sizeOfData = (uint16_t) packet.size();
        std::vector<uint8_t> finalPacket;
        finalPacket.insert(finalPacket.end(), (uint8_t *) &type2Frame, ((uint8_t *) &type2Frame) + sizeof type2Frame);
        finalPacket.insert(finalPacket.end(), packet.begin(), packet.end());
        sendCallback(finalPacket);
        superFrameNo++;
        return EdgewareFrameMessages::noError;
    }

    uint16_t fragmentNo = 0;
    EdgewareFrameType1 type1Frame;
    type1Frame.dataContent = dataContent;
    type1Frame.superFrameNo = superFrameNo;
    size_t dataPayload = (uint16_t) (currentMTU - sizeof(EdgewareFrameType1));

    uint64_t dataPointer = 0;

    size_t diffFrames = sizeof(EdgewareFrameType2) - sizeof(EdgewareFrameType1);
    uint16_t ofFragmentNo =
            ceil((double) (packet.size() + diffFrames) / (double) (currentMTU - sizeof(EdgewareFrameType1))) - 1;
    type1Frame.ofFragmentNo = ofFragmentNo;

    for (; fragmentNo < ofFragmentNo; fragmentNo++) {
        type1Frame.fragmentNo = fragmentNo;
        std::vector<uint8_t> finalPacket;
        finalPacket.insert(finalPacket.end(), (uint8_t *) &type1Frame, ((uint8_t *) &type1Frame) + sizeof type1Frame);
        finalPacket.insert(finalPacket.end(), packet.begin() + dataPointer, packet.begin() + dataPointer + dataPayload);
        dataPointer += dataPayload;
        sendCallback(finalPacket);
    }

    size_t dataLeftToSend = packet.size() - dataPointer;
    //Debug me for calculation errors
    if (dataLeftToSend + sizeof(EdgewareFrameType2) > currentMTU) {
        LOGGER(true, LOGG_FATAL, "Calculation bug.. Value that made me sink -> " << packet.size());
        return EdgewareFrameMessages::internalCalculationError;
    }

    EdgewareFrameType2 type2Frame;
    type2Frame.superFrameNo = superFrameNo;
    type2Frame.fragmentNo = fragmentNo;
    type2Frame.ofFragmentNo = ofFragmentNo;
    type2Frame.dataContent = dataContent;
    type2Frame.sizeOfData = (uint16_t) dataLeftToSend;
    std::vector<uint8_t> finalPacket;
    finalPacket.insert(finalPacket.end(), (uint8_t *) &type2Frame, ((uint8_t *) &type2Frame) + sizeof type2Frame);
    finalPacket.insert(finalPacket.end(), packet.begin() + dataPointer, packet.begin() + dataPointer + dataLeftToSend);
    sendCallback(finalPacket);

    superFrameNo++;
    return EdgewareFrameMessages::noError;
}