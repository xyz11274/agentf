//
// Created by z00913804 on 25-5-21.
//
#include "taskManage/TaskNode.h"

TaskNode::TaskNode(std::shared_ptr<Task> t) {
    task = t;
}

void TaskNode::execTask(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl) {
    task->exec( accessCtrl );
}