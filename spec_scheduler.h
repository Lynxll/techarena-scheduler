#ifndef TECHARENA_SPEC_SCHEDULER_H
#define TECHARENA_SPEC_SCHEDULER_H

#include "spec_system.h"
#include <memory>
#include <tuple>
#include <vector>

namespace arena {

// ==================================================
// The atomic decision produced by the scheduler at each
// scheduling point. Implementation is up to contestant.
// If job_id is 0, the running job on the given thread is
// preempted without replacement.
// ==================================================
struct SchedulerDecision {
    std::uint16_t job_set_id;    // Job set id (ignored when job_id == 0)
    std::uint16_t job_id;        // Job id within the set (0 = preemption without replacement)
    std::uint8_t  core_id;       // Target core
    std::uint8_t  thread_id;     // Target thread within the core
    std::uint64_t start_time_ns; // Planned start time
};

class IScheduler {
public:
    virtual ~IScheduler() = default;
    virtual void set_system_spec(const SystemSpec& spec) = 0;
    virtual uint64_t arrive_at(uint64_t time, const std::vector<JobSetSpec>& new_job_sets) = 0;
    virtual std::tuple<std::vector<SchedulerDecision>, uint64_t> commit_until(uint64_t time) = 0;
    virtual void terminate() = 0;
};

std::unique_ptr<IScheduler> create_scheduler();

} // namespace arena

#endif // TECHARENA_SPEC_SCHEDULER_H
