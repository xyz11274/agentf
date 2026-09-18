#pragma once

#include "runtime_policy.pb.h"
#include <string>
#include <vector>

namespace trajectory::evolution {

void validate_schedule_rules(const v1::RuntimeOptions& options);
v1::SchedulePlan plan_schedule(const v1::RuntimeOptions& options,
                               const v1::RuntimeQuery& query);

// Persist this state per batch. Callers serialize updates using their existing
// run transaction; no worker should occupy an execution slot waiting on a peer.
v1::ScheduleState begin_schedule(const v1::SchedulePlan& plan);
std::vector<std::string> ready_schedule(const v1::ScheduleState& state);
v1::ScheduleState advance_schedule(v1::ScheduleState state,
                                    const std::string& action_id,
                                    const std::string& status);
bool schedule_finished(const v1::ScheduleState& state);

}  // namespace trajectory::evolution
