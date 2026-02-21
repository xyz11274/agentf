#ifndef AGENTFLOW_RUNTIME_SHARED_RUNTIME_STATE_H
#define AGENTFLOW_RUNTIME_SHARED_RUNTIME_STATE_H

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace agentflow {
namespace runtime {

struct WorkflowInstanceInfo {
  std::string workflow_instance_id;
  std::string workflow_id;
  std::string agent_object_id;
  std::string agent_instance_id;
  std::string worker_id;
  int         ref_count = 0;
};

class IRuntimeSharedState {
public:
  virtual ~IRuntimeSharedState() = default;

  virtual void add_or_update_workflow_instance(const WorkflowInstanceInfo& info) = 0;
  virtual bool remove_workflow_instance(const std::string& workflow_instance_id) = 0;
  virtual bool get_workflow_instance(const std::string& workflow_instance_id,
                                     WorkflowInstanceInfo& out) const = 0;
  virtual std::vector<WorkflowInstanceInfo> list_workflow_instances() const = 0;
  virtual int increment_workflow_ref(const std::string& workflow_instance_id) = 0;
  virtual int decrement_workflow_ref(const std::string& workflow_instance_id) = 0;
};

class InMemoryRuntimeSharedState : public IRuntimeSharedState {
public:
  void add_or_update_workflow_instance(const WorkflowInstanceInfo& info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    workflow_instances_[info.workflow_instance_id] = info;
  }

  bool remove_workflow_instance(const std::string& workflow_instance_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return workflow_instances_.erase(workflow_instance_id) > 0;
  }

  bool get_workflow_instance(const std::string& workflow_instance_id,
                             WorkflowInstanceInfo& out) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = workflow_instances_.find(workflow_instance_id);
    if(it == workflow_instances_.end()) {
      return false;
    }
    out = it->second;
    return true;
  }

  std::vector<WorkflowInstanceInfo> list_workflow_instances() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WorkflowInstanceInfo> infos;
    infos.reserve(workflow_instances_.size());
    for(const auto& kv : workflow_instances_) {
      infos.push_back(kv.second);
    }
    return infos;
  }

  int increment_workflow_ref(const std::string& workflow_instance_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = workflow_instances_.find(workflow_instance_id);
    if(it == workflow_instances_.end()) {
      return -1;
    }
    return ++it->second.ref_count;
  }

  int decrement_workflow_ref(const std::string& workflow_instance_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = workflow_instances_.find(workflow_instance_id);
    if(it == workflow_instances_.end()) {
      return -1;
    }
    if(it->second.ref_count > 0) {
      --it->second.ref_count;
    }
    return it->second.ref_count;
  }

private:
  mutable std::mutex                         mutex_;
  std::map<std::string, WorkflowInstanceInfo> workflow_instances_;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_SHARED_RUNTIME_STATE_H
