//
// Created by Anders Cedronius on 2019-11-11.
//

#include "EdgewareFrameProtocol.h"
#include "EdgewareInternal.h"

//Constructor setting the MTU (Only needed if sending, mode == packer)
//Limit the MTU to uint16_t MAX and 255 min //The upper limit is hard
// the lower limit is actually type2frameSize+1, keep it at 255 for now
EdgewareFrameProtocol::EdgewareFrameProtocol(uint16_t setMTU, EdgewareFrameMode mode) {
    if (setMTU > USHRT_MAX) {
        LOGGER(true, LOGG_FATAL, "MTU Larger than " << unsigned(USHRT_MAX) << " MTU not supported");
        currentMTU = USHRT_MAX;
    } else if ((setMTU < UINT8_MAX) && mode != EdgewareFrameMode::unpacker) {
        LOGGER(true, LOGG_FATAL, "MTU lower than " << unsigned(UINT8_MAX) << " is not accepted.");
        currentMTU = UINT8_MAX;
    } else {
        currentMTU = setMTU;
    }
    threadActive = false;
    isThreadActive = false;
    sendCallback = std::bind(&EdgewareFrameProtocol::sendData, this, std::placeholders::_1);
    recieveCallback = std::bind(&EdgewareFrameProtocol::gotData, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5);
    LOGGER(true, LOGG_NOTIFY, "EdgewareFrameProtocol constructed");
}


EdgewareFrameProtocol::~EdgewareFrameProtocol() {
    //If my worker is active we need to stop it.
    if (threadActive) {
        stopUnpacker();
    }
    LOGGER(true, LOGG_NOTIFY, "EdgewareFrameProtocol destruct");
}

//Dummy callback for transmitter
void EdgewareFrameProtocol::sendData(const std::vector<uint8_t> &subPacket) {
    LOGGER(true, LOGG_ERROR, "Implement the sendCallback method for the protocol to work.");
}

//Dummy callback for reciever
void EdgewareFrameProtocol::gotData(EdgewareFrameProtocol::framePtr &packet, EdgewareFrameContent content, bool broken,
                                    uint64_t pts, uint32_t code) {
    LOGGER(true, LOGG_ERROR, "Implement the recieveCallback method for the protocol to work.");
}

//This method is generating a linear uint64_t counter from the nonlinear uint16_t
//counter. The maximum loss / hole this calculator can handle is (INT16_MAX)

uint64_t EdgewareFrameProtocol::superFrameRecalculator(uint16_t superFrame) {
    if (superFrameFirstTime) {
        oldSuperframeNumber = superFrame;
        superFrameRecalc = superFrame;
        superFrameFirstTime = false;
        return superFrameRecalc;
    }

    int16_t changeValue = (int16_t)superFrame - (int16_t)oldSuperframeNumber;
    int64_t cval = (int64_t)changeValue;

    //std::cout << "1 -> " << unsigned(superFrame) <<
    //" " << signed(changeValue) <<
    //" " << signed(cval) <<
    //std::endl;

    oldSuperframeNumber = superFrame;

    if (cval > INT16_MAX) {
        cval -= (UINT16_MAX-1);
        superFrameRecalc = superFrameRecalc - cval;
    } else {
        superFrameRecalc = superFrameRecalc + cval;
    }
    return superFrameRecalc;
}

//Unpack method for type1 packets. Type1 packets are the parts of frames larger than the MTU
EdgewareFrameMessages EdgewareFrameProtocol::unpackType1(const std::vector<uint8_t> &subPacket) {
    std::lock_guard<std::mutex> lock(netMtx);

    EdgewareFrameType1 type1Frame = *(EdgewareFrameType1 *) subPacket.data();
    Bucket *thisBucket = &bucketList[type1Frame.superFrameNo & 0b1111111111111];

    //is this entry in the buffer active? If no, create a new else continue filling the bucket with data.
    if (!thisBucket->active) {
        //LOGGER(false,LOGG_NOTIFY,"Setting: " << unsigned(type1Frame.superFrameNo));
        thisBucket->active = true;
        thisBucket->savedSuperFrameNo = type1Frame.superFrameNo;
        thisBucket->haveRecievedPacket.reset();
        thisBucket->pts = UINT64_MAX;
        thisBucket->code = UINT32_MAX;
        thisBucket->haveRecievedPacket[type1Frame.fragmentNo] = 1;
        thisBucket->deliveryOrder = superFrameRecalculator(type1Frame.superFrameNo);
        std::cout << "in -> " << unsigned(type1Frame.superFrameNo) << " recalc -> " << unsigned(thisBucket->deliveryOrder) << std::endl;
        thisBucket->dataContent = type1Frame.dataContent;
        thisBucket->timeout = bucketTimeout;
        thisBucket->fragmentCounter = 0;
        thisBucket->ofFragmentNo = type1Frame.ofFragmentNo;
        thisBucket->fragmentSize = (subPacket.size() - sizeof(EdgewareFrameType1));
        size_t insertDataPointer = thisBucket->fragmentSize * type1Frame.fragmentNo;
        thisBucket->bucketData = std::make_shared<allignedFrameData>(
                thisBucket->fragmentSize * (type1Frame.ofFragmentNo + 1));

        if (thisBucket->bucketData->framedata == nullptr) {
            thisBucket->active = false;
            return EdgewareFrameMessages::memoryAllocationError;
        }

        std::memcpy(thisBucket->bucketData->framedata + insertDataPointer,
                    subPacket.data() + sizeof(EdgewareFrameType1), subPacket.size() - sizeof(EdgewareFrameType1));
        return EdgewareFrameMessages::noError;
    }

    //there is a gap in recieving the packets. Increase the bucket size list.. if the
    //bucket size list is == X*UINT16_MAX you will no longer detect any buffer errors
    if (type1Frame.superFrameNo != thisBucket->savedSuperFrameNo) {
        return EdgewareFrameMessages::bufferOutOfResources;
    }

    //I'm getting a packet with data larger than the expected size
    //this can be generated by wraparound in the bucket bucketList
    //The notification about more than 50% buffer full level should already
    //be triggered by now.
    //I invalidate this bucket to save me but the user should be notified somehow about this state. FIXME

    if (thisBucket->ofFragmentNo < type1Frame.fragmentNo || type1Frame.ofFragmentNo != thisBucket->ofFragmentNo) {
        LOGGER(true, LOGG_FATAL, "bufferOutOfBounds");
        thisBucket->active = false;
        return EdgewareFrameMessages::bufferOutOfBounds;
    }

    //FIXME 1+1 mode not supported need a window of packets already dealt width
    //Have I already recieved this packet before? (duplicate?)
    if (thisBucket->haveRecievedPacket[type1Frame.fragmentNo] == 1) {
        return EdgewareFrameMessages::duplicatePacketRecieved;
    } else {
        thisBucket->haveRecievedPacket[type1Frame.fragmentNo] = 1;
    }

    //Let's re-set the timout and let also add +1 to the fragment counter
    thisBucket->timeout = bucketTimeout;
    thisBucket->fragmentCounter++;

    //move the data to the correct fragment position in the frame.
    //A bucket contains the frame data -> This is the internal data format
    // |bucket start|information about the frame|bucket end| in the bucket there is a pointer to the actual data named framePtr this is the structure there ->
    // linear array of -> |fragment start|fragment data|fragment end|
    // insertDataPointer will point to the fragment start above and fill with the incomming data

    size_t insertDataPointer = thisBucket->fragmentSize * type1Frame.fragmentNo;
    std::memcpy(thisBucket->bucketData->framedata + insertDataPointer, subPacket.data() + sizeof(EdgewareFrameType1),
                subPacket.size() - sizeof(EdgewareFrameType1));
    return EdgewareFrameMessages::noError;
}

// Unpack method for type2 packets. Where we know there is also type 1 packets involved.
// Type2 packets are parts of frames smaller than the MTU
// The data IS the last data of a sequence
// See the comments from above.

EdgewareFrameMessages EdgewareFrameProtocol::unpackType2LastFrame(const std::vector<uint8_t> &subPacket) {
    std::lock_guard<std::mutex> lock(netMtx);
    EdgewareFrameType2 type2Frame = *(EdgewareFrameType2 *) subPacket.data();
    Bucket *thisBucket = &bucketList[type2Frame.superFrameNo & 0b1111111111111];

    if (!thisBucket->active) {
        thisBucket->active = true;
        thisBucket->savedSuperFrameNo = type2Frame.superFrameNo;
        thisBucket->haveRecievedPacket.reset();
        thisBucket->pts = type2Frame.pts;
        thisBucket->code = type2Frame.code;
        thisBucket->haveRecievedPacket[type2Frame.fragmentNo] = 1;
        thisBucket->deliveryOrder = superFrameRecalculator(type2Frame.superFrameNo);
        std::cout << "GAP ->" << unsigned(thisBucket->deliveryOrder) << std::endl;
        thisBucket->dataContent = type2Frame.dataContent;
        thisBucket->timeout = bucketTimeout;
        thisBucket->ofFragmentNo = type2Frame.ofFragmentNo;
        thisBucket->fragmentCounter = 0;
        thisBucket->fragmentSize = type2Frame.type1PacketSize;
        size_t reserveThis = ((thisBucket->fragmentSize * type2Frame.ofFragmentNo) +
                              (subPacket.size() - sizeof(EdgewareFrameType2)));
        thisBucket->bucketData = std::make_shared<allignedFrameData>(reserveThis);
        if (thisBucket->bucketData->framedata == nullptr) {
            thisBucket->active = false;
            return EdgewareFrameMessages::memoryAllocationError;
        }
        size_t insertDataPointer = type2Frame.type1PacketSize * type2Frame.fragmentNo;
        std::memcpy(thisBucket->bucketData->framedata + insertDataPointer,
                    subPacket.data() + sizeof(EdgewareFrameType2), subPacket.size() - sizeof(EdgewareFrameType2));
        return EdgewareFrameMessages::noError;
    }

    if (type2Frame.superFrameNo != thisBucket->savedSuperFrameNo) {
        return EdgewareFrameMessages::bufferOutOfResources;
    }

    if (thisBucket->ofFragmentNo < type2Frame.fragmentNo || type2Frame.ofFragmentNo != thisBucket->ofFragmentNo) {
        LOGGER(true, LOGG_FATAL, "bufferOutOfBounds");
        thisBucket->active = false;
        return EdgewareFrameMessages::bufferOutOfBounds;
    }

    if (thisBucket->haveRecievedPacket[type2Frame.fragmentNo] == 1) {
        return EdgewareFrameMessages::duplicatePacketRecieved;
    } else {
        thisBucket->haveRecievedPacket[type2Frame.fragmentNo] = 1;
    }

    //Type 2 frames contains the pts and code. If for some reason the type2 packet is missing or the frame is delivered
    //Before the type2 frame arrives PTS and CODE are set to it's respective 'illegal' value. meaning you cant't use them.
    thisBucket->timeout = bucketTimeout;
    thisBucket->pts = type2Frame.pts;
    thisBucket->code = type2Frame.code;
    thisBucket->fragmentCounter++;

    //when the type2 frame is recieved only then is the actual size to be delivered known... Now set it for the bucketData
    thisBucket->bucketData->frameSize =
            (thisBucket->fragmentSize * type2Frame.ofFragmentNo) + (subPacket.size() - sizeof(EdgewareFrameType2));

    //Type 2 is always at the end and is always the highest number fragment
    size_t insertDataPointer = type2Frame.type1PacketSize * type2Frame.fragmentNo;
    std::memcpy(thisBucket->bucketData->framedata + insertDataPointer, subPacket.data() + sizeof(EdgewareFrameType2),
                subPacket.size() - sizeof(EdgewareFrameType2));
    return EdgewareFrameMessages::noError;
}


// This is the thread going trough the buckets to see if they should be delivered to
// the 'user'
// 1000 times per second is a bit aggressive, change to 100 times per second? FIXME, talk about what is realistic.. Settable? static? limits? why? for what reason?
void EdgewareFrameProtocol::unpackerWorker(uint32_t timeout) {
    //Set the defaults. meaning the thread is running and there is no head of line blocking action going on.
    threadActive = true;
    isThreadActive = true;
    bool foundHeadOfLineBlocking = false;
    bool fistDelivery = headOfLineBlockingTimeout?false:true; //if hol is used then we must recieve at least two packets first to know where to start counting.
    uint32_t headOfLineBlockingCounter = 0;
    uint64_t headOfLineBlockingTail = 0;
    uint64_t expectedNextFrameToDeliver = 0;

    uint64_t oldestFrameDelivered = 0;

    while (threadActive) {
        usleep(1000); //Check all active buckets each milisecond
        bool timeOutTrigger = false;
        uint32_t activeCount = 0;
        std::vector<CandidateToDeliver> candidates;
        uint64_t deliveryOrderOldest = UINT64_MAX;

        //The default mode is not to clear any buckets
        bool clearHeadOfLineBuckets = false;
        //If I'm in head of blocking garbage collect mode.
        if (foundHeadOfLineBlocking) {
            //If some one instructed me to timeout then let's timeout first
            if (headOfLineBlockingCounter) {
                headOfLineBlockingCounter--;
            } else {
                //Timeout triggered.. Let's garbage collect the head.
                clearHeadOfLineBuckets = true;
                foundHeadOfLineBlocking = false;
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
                    expectedNextFrameToDeliver = deliveryOrderOldest;
                }

                //Are we cleaning out old buckets and did we found a head to timout?
                if ((bucketList[i].deliveryOrder <= headOfLineBlockingTail) && clearHeadOfLineBuckets) {
                    bucketList[i].timeout = 1;
                }

                bucketList[i].timeout--;

                //If the bucket is ready to be delivered or is the bucket timedout?
                if (!bucketList[i].timeout) {
                    timeOutTrigger = true;
                    //std::cout << "bip!" << unsigned(bucketList[i].fragmentCounter) << " " << unsigned(bucketList[i].ofFragmentNo) << std::endl;
                    candidates.push_back(CandidateToDeliver(bucketList[i].deliveryOrder, i,
                                                            bucketList[i].fragmentCounter != bucketList[i].ofFragmentNo, bucketList[i].pts,
                                                            bucketList[i].code));
                    bucketList[i].timeout = 1; //We want to timeout this again if head of line blocking is on
                } else if (bucketList[i].fragmentCounter == bucketList[i].ofFragmentNo) {
                    std::cout << "GAG ->" << unsigned(bucketList[i].deliveryOrder) << std::endl;
                    candidates.push_back(CandidateToDeliver(bucketList[i].deliveryOrder, i, false, bucketList[i].pts,
                                                            bucketList[i].code));
                }
            }
        }

        size_t numCandidatesToDeliver=candidates.size();
        if ((!fistDelivery && numCandidatesToDeliver >= 2) || timeOutTrigger) {
            fistDelivery=true;
            //std::cout << "bim!" << std::endl;
            expectedNextFrameToDeliver=deliveryOrderOldest;
        }

        //Do we got any timedout buckets or finished buckets?
        if (numCandidatesToDeliver && fistDelivery) {
            //std::cout << "pao!" << std::endl;
            //Sort them in delivery order
            std::sort(candidates.begin(), candidates.end(), sortDeliveryOrder());


            for (auto &x: candidates) {
                std::cout << ">>>" << unsigned(x.deliveryOrder) << " is broken " << x.broken << std::endl;
            }

            //So ok we have cleared the head send it all out
            if (clearHeadOfLineBuckets) {
                for (auto &x: candidates) {
                    if (oldestFrameDelivered <= x.deliveryOrder) {
                        oldestFrameDelivered = x.deliveryOrder;
                        recieveCallback(bucketList[x.bucket].bucketData, bucketList[x.bucket].dataContent, x.broken,
                                        x.pts,
                                        x.code);
                    }
                    bucketList[x.bucket].active = false;
                    bucketList[x.bucket].bucketData = nullptr;
                }
            } else {

                //in this run we have not cleared the head.. is there a head to clear?
                //We can't be in waitning for timout and we can't have a 0 time-out
                //A 0 timout means out of order delivery else we-re here.
                //So in out of order delivery we time out the buckets instead of flushing the head.

                //Check for head of line blocking only if HOL-timoeut is set
                if (deliveryOrderOldest < bucketList[candidates[0].bucket].deliveryOrder && headOfLineBlockingTimeout &&
                    !foundHeadOfLineBlocking) {
                    LOGGER(false, LOGG_NOTIFY, "HOL found"); //FIXME-REMOVE
                    foundHeadOfLineBlocking = true; //Found hole
                    headOfLineBlockingCounter = headOfLineBlockingTimeout; //Number of times to spin this loop
                    headOfLineBlockingTail = bucketList[candidates[0].bucket].deliveryOrder; //This is the tail
                }

                //Deliver only when head of line blocking is cleared and we're back to normal
                if (!foundHeadOfLineBlocking) {
                    for (auto &x: candidates) {
                        if (expectedNextFrameToDeliver != x.deliveryOrder && headOfLineBlockingTimeout) {
                            LOGGER(false, LOGG_NOTIFY, "HOL found2");
                            foundHeadOfLineBlocking = true; //Found hole
                            headOfLineBlockingCounter = headOfLineBlockingTimeout; //Number of times to spin this loop
                            headOfLineBlockingTail = x.deliveryOrder; //So we basically give the non existing data a chance to arrive..
                            break;
                        }
                        expectedNextFrameToDeliver++;

                        if (oldestFrameDelivered <= x.deliveryOrder) {
                            LOGGER(false, LOGG_NOTIFY, "a " << unsigned(oldestFrameDelivered))
                            oldestFrameDelivered = x.deliveryOrder;
                            recieveCallback(bucketList[x.bucket].bucketData, bucketList[x.bucket].dataContent, x.broken,
                                            x.pts, x.code);
                        }
                        bucketList[x.bucket].active = false;
                        bucketList[x.bucket].bucketData = nullptr;
                    }
                }
            }
        }
        netMtx.unlock();

        //Is more than 50% of the buffer used... Keep track of this. 50% might be a bit low. FIXME set to 80% and warn user.
        if (activeCount > UINT8_MAX / 2) {
            LOGGER(true, LOGG_FATAL, "Current active buckets are more than half the circular buffer.");
        }

    }
    isThreadActive = false;
}

//Start reciever worker thread
EdgewareFrameMessages EdgewareFrameProtocol::startUnpacker(uint32_t bucketTimeoutMaster, uint32_t holTimeoutMaster) {
    if (isThreadActive) {
        LOGGER(true, LOGG_FATAL, "Unpacker already working");
        return EdgewareFrameMessages::unpackerAlreadyStarted;
    }
    if (bucketTimeoutMaster == 0) {
        LOGGER(true, LOGG_FATAL, "bucketTimeoutMaster can't be 0");
        return EdgewareFrameMessages::parameterError;
    }
    if (holTimeoutMaster>=bucketTimeoutMaster) {
        LOGGER(true, LOGG_FATAL, "holTimeoutMaster cant be less or equal to bucketTimeoutMaster");
        return EdgewareFrameMessages::parameterError;
    }

    bucketTimeout = bucketTimeoutMaster;
    headOfLineBlockingTimeout = holTimeoutMaster;
    std::thread(std::bind(&EdgewareFrameProtocol::unpackerWorker, this, bucketTimeoutMaster)).detach();
    return EdgewareFrameMessages::noError;
}

//Stop reciever worker thread
EdgewareFrameMessages EdgewareFrameProtocol::stopUnpacker() {
    //Set the semaphore to stop thread
    threadActive = false;
    uint32_t lockProtect = 1000;
    //check for it to actually stop
    while (isThreadActive) {
        usleep(1000);
        if (!--lockProtect) {
            //we gave it a second now exit anyway
            LOGGER(true, LOGG_FATAL, "unpackerWorker thread not stopping. Quitting anyway");
            return EdgewareFrameMessages::failedStoppingUnpacker;
        }
    }
    return EdgewareFrameMessages::noError;
}

//Unpack method. We recieved a fragment of data or a full frame. Lets unpack it
EdgewareFrameMessages EdgewareFrameProtocol::unpack(const std::vector<uint8_t> &subPacket) {
    //Type 0 packet. Discard and continue
    //Type 0 packets can be used to fill with user data outside efp protocol packets just put a uint8_t = Frametype::type0 at position 0 and then any data.
    //Type 1 are packets larger than MTU
    //Type 2 are packets smaller than MTU
    //Type 2 packets are also used at the end of Type 1 packet superFrames

    if (subPacket[0] == Frametype::type0) {
        return EdgewareFrameMessages::noError;
    } else if (subPacket[0] == Frametype::type1) {
        return unpackType1(subPacket);
    } else if (subPacket[0] == Frametype::type2) {
        if (subPacket.size() < sizeof(EdgewareFrameType2)) {
            return EdgewareFrameMessages::framesizeMismatch;
        }
        EdgewareFrameType2 type2Frame = *(EdgewareFrameType2 *) subPacket.data();
        if (type2Frame.ofFragmentNo == type2Frame.fragmentNo) {
            return unpackType2LastFrame(subPacket);
        } else {
            return EdgewareFrameMessages::endOfPacketError;
        }
    }

    //did not catch anything I understand
    return EdgewareFrameMessages::unknownFrametype;
}

//Pack data method. Fragments the data and calls the sendCallback method at the host level.
EdgewareFrameMessages
EdgewareFrameProtocol::packAndSend(const std::vector<uint8_t> &packet, EdgewareFrameContent dataContent, uint64_t pts,
                                   uint32_t code) {

    if (pts == UINT64_MAX) {
        return EdgewareFrameMessages::reservedPTSValue;
    }

    if (code == UINT32_MAX) {
        return EdgewareFrameMessages::reservedCodeValue;
    }

    //Will the data fit?
    //we know that we can send USHRT_MAX (65535) packets
    //the last packet will be a type2 packet.. so the current MTU muliplied with USHRT_MAX subtracting the space the protocol needs for the headers
    if (packet.size() > (((currentMTU - sizeof(EdgewareFrameType1)) * (USHRT_MAX - 1)) + (currentMTU - sizeof(EdgewareFrameType2))) ) {
        return EdgewareFrameMessages::tooLargeFrame;
    }

    if ((packet.size() + sizeof(EdgewareFrameType2)) <= currentMTU) {
        EdgewareFrameType2 type2Frame;
        type2Frame.superFrameNo = superFrameNoGenerator;
        type2Frame.dataContent = dataContent;
        type2Frame.sizeOfData = (uint16_t) packet.size(); //The total size fits uint16_t since we cap the MTU to uint16_t
        type2Frame.pts = pts;
        type2Frame.code = code;
        std::vector<uint8_t> finalPacket;
        finalPacket.insert(finalPacket.end(), (uint8_t *) &type2Frame, ((uint8_t *) &type2Frame) + sizeof type2Frame);
        finalPacket.insert(finalPacket.end(), packet.begin(), packet.end());
        sendCallback(finalPacket);
        superFrameNoGenerator++;
        return EdgewareFrameMessages::noError;
    }

    uint16_t fragmentNo = 0;
    EdgewareFrameType1 type1Frame;
    type1Frame.dataContent = dataContent;
    type1Frame.superFrameNo = superFrameNoGenerator;
    //The size is known for type1 packets no need to write it in any header.
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

    //Create the last type2 packet
    size_t dataLeftToSend = packet.size() - dataPointer;
    //Debug me for calculation errors
    if (dataLeftToSend + sizeof(EdgewareFrameType2) > currentMTU) {
        LOGGER(true, LOGG_FATAL, "Calculation bug.. Value that made me sink -> " << packet.size());
        return EdgewareFrameMessages::internalCalculationError;
    }
    EdgewareFrameType2 type2Frame;
    type2Frame.superFrameNo = superFrameNoGenerator;
    type2Frame.fragmentNo = fragmentNo;
    type2Frame.ofFragmentNo = ofFragmentNo;
    type2Frame.dataContent = dataContent;
    type2Frame.sizeOfData = (uint16_t) dataLeftToSend;
    type2Frame.pts = pts;
    type2Frame.code = code;
    type2Frame.type1PacketSize = currentMTU - sizeof(type1Frame);
    std::vector<uint8_t> finalPacket;
    finalPacket.insert(finalPacket.end(), (uint8_t *) &type2Frame, ((uint8_t *) &type2Frame) + sizeof type2Frame);
    finalPacket.insert(finalPacket.end(), packet.begin() + dataPointer, packet.begin() + dataPointer + dataLeftToSend);
    sendCallback(finalPacket);
    superFrameNoGenerator++;
    return EdgewareFrameMessages::noError;
}

//Used by the unit tests
size_t EdgewareFrameProtocol::geType1Size() {
    return sizeof(EdgewareFrameType1);
}

size_t EdgewareFrameProtocol::geType2Size() {
    return sizeof(EdgewareFrameType2);
}