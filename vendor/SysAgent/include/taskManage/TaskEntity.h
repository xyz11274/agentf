//
// Created by z00913804 on 25-5-21.
//

#ifndef SYSAGENT_TASKENTITY_H
#define SYSAGENT_TASKENTITY_H
#include "string"
class TaskEntity {

public:
    std::string taskID;
    std::string agentName;
    std::string requestContent;
    TaskEntity(std::string &parentTaskID, std::string &agentName, std::string &requestContent) {
        this->taskID = parentTaskID;
        this->agentName = agentName;
        this->requestContent = requestContent;
    }
};
#endif //SYSAGENT_TASKENTITY_H
