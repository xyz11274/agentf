//
// Created by z00913804 on 25-5-20.
//

#ifndef SYSAGENT_TASKNODE_H
#define SYSAGENT_TASKNODE_H

#include "Task.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
class TaskNode {
public:
    explicit TaskNode(std::shared_ptr<Task> task);
    std::vector<shared_ptr<TaskNode>> childrenNode;
    shared_ptr<Task> task;
    void execTask(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);
};
#endif //SYSAGENT_TASKNODE_H
