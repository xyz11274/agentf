/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */
//
// Created by z00913804 on 25-12-8.
//

#ifndef SYSAGENT_TIMER_TASK_H
#define SYSAGENT_TIMER_TASK_H
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <functional>
class TimerTask {
public:
    TimerTask(int intervalS, const std::function<void()>& task)
        : intervalS_(intervalS), task_(task), running_(false) {}

    void start()
    {
        running_ = true;
        thread_ = std::thread([this]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::seconds(intervalS_));
                if (!running_) break;
                task_();
            }
        });
    }

    void stop()
    {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    int intervalS_;
    std::function<void()> task_;
    std::atomic<bool> running_;
    std::thread thread_;
};
#endif // SYSAGENT_TIMER_TASK_H
