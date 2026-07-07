//
// Created by z00913804 on 25-8-14.
//
#include "queue/KmmMsgReadQueueHandler.h"
#include "TeHelper.h"
#include "mm_sdk.h"
#include "data_source.h"
#include "memory/History.h"
#include <thread>
#include <utility>
#define MEMORY_BATCH_SIZE 30
#define TIMEOUT_MS 5000
// 注意 http2的发送buff
void KmmMsgReadQueueHandler::start()
{
    LOG_INFO("KmmMsgReadQueueHandler::start processing kmm message queue");
    while (true) {
        try {
            LOG_INFO_EX("KmmMsgReadQueueHandler processing kmm message queue");
            auto msgs = msgQueue.GetMessages(MEMORY_BATCH_SIZE, std::chrono::milliseconds(TIMEOUT_MS));
            if (msgs.empty()) {
                LOG_INFO("Have no memory in the queue");
                continue;
            }
            std::string sizeStr = std::to_string(msgs.size());
            LOG_INFO_EX("给KMM发一批, 数量是 [%s]", sizeStr.c_str());
            addKmmMemoryBatchHandler(msgs);
        } catch (const std::exception& e) {
            LOG_ERR_EX("Exception occurred while processing message: %s", e.what());
        }
    }
}

void KmmMsgReadQueueHandler::addKmmMemoryBatchHandler(std::vector<Memory>& msgs)
{
    BatchMemory batchMemory;
    batchMemory.metadata = "";
    batchMemory.contents = std::move(msgs);
    addBatchMemory(batchMemory);
}

void KmmMsgReadQueueHandler::postData(const Memory& message)
{
    msgQueue.PostMessage(message);
}
