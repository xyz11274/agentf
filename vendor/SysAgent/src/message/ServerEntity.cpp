//
// Created by c00888021 on 2025/11/11.
//

#include "message/ServerEntity.h"

std::map<lws*, std::queue<std::shared_ptr<std::string>>> sessionQueues;
std::mutex queuesMutex;
std::unordered_map<lws*, std::shared_ptr<PerSession>> perSessionMap;
std::mutex perSessionMapMutex;