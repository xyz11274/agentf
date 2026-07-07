//
// Created by l00889076 on 2026/3/30.
//

#ifndef SYSAGENT_SYSAGENT_H
#define SYSAGENT_SYSAGENT_H

#include <utility>

#include "taskManage/Agent.h"

class SystemAgent : public Agent {
public:
    SystemAgent(std::string type, std::string name, std::string desc, std::string url, std::string port,
        std::string baseUrl = "") :
        Agent(std::move(type),std::move(name),std::move(desc),std::move(url),std::move(port),std::move(baseUrl))
    {

    }
    bool sendRestfulRequest(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) const override;
};


#endif // SYSAGENT_SYSAGENT_H
