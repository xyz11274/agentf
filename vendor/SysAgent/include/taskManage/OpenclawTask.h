//
// Created by c00888021 on 2025/11/22.
//

#ifndef SYSAGENT_OPENCLAWTASK_H
#define SYSAGENT_OPENCLAWTASK_H

#include "Task.h"

class OpenclawTask : public Task, public std::enable_shared_from_this<OpenclawTask> {
public:
    explicit OpenclawTask(std::string &taskID, std::string &agentName, std::string &requestContent);

    void exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl) override;

    void stop(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl) override;
};


#endif // SYSAGENT_OPENCLAWTASK_H
