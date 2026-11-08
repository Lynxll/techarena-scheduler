#include "spec_scheduler.h"
#include "execution_model.h"
#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>

namespace arena {
namespace {

enum class State { pending, ready, running, completed, failed };

struct LatestStart {
    std::uint64_t deadline_ns;
    double required_ns;

    LatestStart(std::uint64_t deadline = 0, double required = 0)
        : deadline_ns(deadline), required_ns(required) {
        if (std::isnan(required) || required < 0) throw std::invalid_argument("Invalid path duration");
        if (std::isfinite(required) && required >= static_cast<double>(mvp::no_wakeup)) {
            throw std::overflow_error("Path duration exceeds timestamp range");
        }
    }
};

bool operator<(const LatestStart& left, const LatestStart& right) {
    if (std::isinf(left.required_ns)) return !std::isinf(right.required_ns);
    if (std::isinf(right.required_ns)) return false;
    double left_whole, right_whole;
    const double left_fraction = std::modf(left.required_ns, &left_whole);
    const double right_fraction = std::modf(right.required_ns, &right_whole);
    const auto left_cost = static_cast<std::uint64_t>(left_whole);
    const auto right_cost = static_cast<std::uint64_t>(right_whole);
    if (left.deadline_ns >= right.deadline_ns) {
        if (left_cost < right_cost) return false;
        const auto deadline_difference = left.deadline_ns - right.deadline_ns;
        const auto cost_difference = left_cost - right_cost;
        if (deadline_difference != cost_difference) return deadline_difference < cost_difference;
    } else {
        if (left_cost > right_cost) return true;
        const auto deadline_difference = right.deadline_ns - left.deadline_ns;
        const auto cost_difference = right_cost - left_cost;
        if (deadline_difference != cost_difference) return deadline_difference > cost_difference;
    }
    return left_fraction > right_fraction;
}

struct Job {
    std::uint16_t set_id;
    std::uint16_t id;
    std::uint8_t priority;
    mvp::Work work;
    std::vector<std::size_t> successors;
    std::vector<std::size_t> eligible_cores;
    std::size_t remaining_parents = 0;
    double shortest_duration = std::numeric_limits<double>::infinity();
    LatestStart latest_start;
    double critical_path = 0;
    std::size_t running_core = 0;
    State state = State::pending;
};

using ReadyKey = std::tuple<LatestStart, std::uint8_t, double, std::uint16_t, std::uint16_t, std::size_t>;
using RejectedWork = std::tuple<std::uint8_t, std::uint8_t, std::uint32_t,
                                std::uint32_t, std::uint32_t, std::uint64_t>;
ReadyKey ready_key(const Job& job) {
    return {job.latest_start, job.priority, -job.critical_path, job.set_id, job.id, job.work.token};
}

class Scheduler final : public IScheduler {
public:
    void set_system_spec(const SystemSpec& spec) override {
        if (model_) throw std::logic_error("System already initialized");
        system_ = spec;
        model_ = std::make_unique<mvp::ExecutionModel>(system_);
        total_threads_ = 0;
        for (const auto& core : system_.cores) total_threads_ += core.threads;
    }

    std::uint64_t arrive_at(std::uint64_t time, const std::vector<JobSetSpec>& new_sets) override {
        synchronize(time);
        for (const auto& set : new_sets) register_set(time, set);
        if (system_.scheduling_budget == 0) return mvp::no_wakeup;
        if (!ready_.empty()) return mvp::saturating_add(time, 1);
        return next_wakeup(time);
    }

    std::tuple<std::vector<SchedulerDecision>, std::uint64_t>
    commit_until(std::uint64_t time) override {
        synchronize(time);
        std::vector<SchedulerDecision> decisions;
        std::set<RejectedWork> rejected;
        for (auto it = ready_.begin(); it != ready_.end();) {
            if (model_->active().size() == total_threads_) break;
            auto& job = jobs_[std::get<5>(*it)];
            const RejectedWork key{job.work.type, job.work.dram_id, job.work.load_kb,
                                   job.work.write_kb, job.work.compute_units, job.work.deadline_ns};
            if (rejected.count(key) != 0) {
                ++it;
                continue;
            }
            if (place(job, time, decisions)) {
                it = ready_.erase(it);
                rejected.clear();
            } else {
                rejected.insert(key);
                ++it;
            }
        }
        return {std::move(decisions), next_wakeup(time)};
    }

    void terminate() override {
        ready_.clear();
        jobs_.clear();
        set_ids_.clear();
        predicted_.clear();
        model_.reset();
        system_ = SystemSpec{};
        last_time_ = 0;
    }

private:
    SystemSpec system_{};
    std::unique_ptr<mvp::ExecutionModel> model_;
    std::vector<Job> jobs_;
    std::set<std::uint16_t> set_ids_;
    std::set<ReadyKey> ready_;
    std::uint64_t last_time_ = 0;
    std::size_t total_threads_ = 0;
    std::vector<mvp::Completion> predicted_;

    std::uint64_t next_wakeup(std::uint64_t time) const {
        if (system_.scheduling_budget == 0 || model_->active().empty()) return mvp::no_wakeup;
        if (!ready_.empty() && model_->active().size() < total_threads_) {
            return mvp::request_after(model_->next_event_time(), time);
        }
        std::uint64_t event = mvp::no_wakeup;
        for (const auto& completion : predicted_) {
            if (jobs_[completion.token].state == State::running) {
                event = std::min(event, completion.rounded_time_ns());
            }
        }
        return mvp::request_after(event, time);
    }

    void make_ready(Job& job) {
        job.state = State::ready;
        ready_.insert(ready_key(job));
    }

    void fail(std::size_t token) {
        std::vector<std::size_t> stack{token};
        while (!stack.empty()) {
            const auto current = stack.back();
            stack.pop_back();
            auto& job = jobs_[current];
            if (job.state == State::failed || job.state == State::completed) continue;
            if (job.state == State::ready) ready_.erase(ready_key(job));
            job.state = State::failed;
            stack.insert(stack.end(), job.successors.begin(), job.successors.end());
        }
    }

    void synchronize(std::uint64_t time) {
        if (!model_) throw std::logic_error("System not initialized");
        if (time < last_time_) throw std::invalid_argument("Scheduler time moved backwards");
        for (const auto& completion : model_->advance_to(time)) {
            auto& job = jobs_[completion.token];
            if (!completion.on_time) {
                fail(completion.token);
                continue;
            }
            job.state = State::completed;
            for (auto child : job.successors) {
                auto& successor = jobs_[child];
                if (successor.state != State::pending) continue;
                if (--successor.remaining_parents == 0) make_ready(successor);
            }
        }
        for (std::size_t i = 0; i < jobs_.size(); ++i) {
            const auto& job = jobs_[i];
            if ((job.state == State::pending || job.state == State::ready) &&
                job.work.deadline_ns <= time) fail(i);
        }
        last_time_ = time;
    }

    void register_set(std::uint64_t time, const JobSetSpec& set) {
        if (set.id == 0 || set.arrival_ns > time || !set_ids_.insert(set.id).second) {
            throw std::invalid_argument("Invalid job set id or arrival");
        }
        const auto begin = jobs_.size();
        std::map<std::uint16_t, std::size_t> indices;
        for (const auto& spec : set.jobs) {
            if (spec.id == 0 || spec.priority > 100 || !indices.emplace(spec.id, jobs_.size()).second) {
                throw std::invalid_argument("Invalid job id or priority");
            }
            Job job{};
            job.set_id = set.id;
            job.id = spec.id;
            job.priority = spec.priority;
            job.work = {jobs_.size(), spec.type, set.dram_id, spec.load_size_kb,
                        spec.write_size_kb, spec.compute_units,
                        mvp::saturating_add(set.arrival_ns, spec.relative_deadline_ns)};
            for (std::size_t core = 0; core < system_.cores.size(); ++core) {
                if (model_->compatible(job.work, core)) {
                    job.eligible_cores.push_back(core);
                    job.shortest_duration = std::min(job.shortest_duration, model_->solo_duration(job.work, core));
                }
            }
            job.latest_start = LatestStart{job.work.deadline_ns, job.shortest_duration};
            job.critical_path = job.shortest_duration;
            jobs_.push_back(std::move(job));
        }
        for (std::size_t local = 0; local < set.jobs.size(); ++local) {
            std::set<std::uint16_t> unique;
            for (auto parent : set.jobs[local].deps) {
                const auto found = indices.find(parent);
                if (found == indices.end() || !unique.insert(parent).second) {
                    throw std::invalid_argument("Invalid or duplicate dependency");
                }
                jobs_[found->second].successors.push_back(begin + local);
                ++jobs_[begin + local].remaining_parents;
            }
        }
        std::vector<std::size_t> indegrees, order;
        std::queue<std::size_t> queue;
        for (std::size_t i = begin; i < jobs_.size(); ++i) {
            indegrees.push_back(jobs_[i].remaining_parents);
            if (jobs_[i].remaining_parents == 0) queue.push(i);
        }
        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();
            order.push_back(current);
            for (auto child : jobs_[current].successors) {
                if (--indegrees[child - begin] == 0) queue.push(child);
            }
        }
        if (order.size() != set.jobs.size()) throw std::invalid_argument("Dependencies contain a cycle");
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            auto& job = jobs_[*it];
            for (auto child : job.successors) {
                const auto& child_bound = jobs_[child].latest_start;
                job.latest_start = std::min(job.latest_start,
                    LatestStart{child_bound.deadline_ns, child_bound.required_ns + job.shortest_duration});
                job.critical_path = std::max(job.critical_path, job.shortest_duration + jobs_[child].critical_path);
            }
        }
        for (auto token : order) {
            auto& job = jobs_[token];
            if (job.state == State::failed) continue;
            if (job.eligible_cores.empty() || job.work.deadline_ns <= time) {
                fail(token);
            } else if (job.remaining_parents == 0) {
                make_ready(job);
            }
        }
    }

    bool place(Job& job, std::uint64_t time, std::vector<SchedulerDecision>& decisions) {
        if (time >= job.work.deadline_ns ||
            job.shortest_duration >= static_cast<double>(job.work.deadline_ns - time)) {
            return false;
        }
        using Placement = std::tuple<double, std::uint8_t, std::size_t, std::uint8_t>;
        std::vector<Placement> candidates;
        for (auto core : job.eligible_cores) {
            const auto thread = model_->free_thread(core);
            if (thread < 0 || model_->reserved_kb(core) + job.work.load_kb + job.work.write_kb >
                              system_.cores[core].sram_capacity_kb) continue;
            candidates.emplace_back(model_->placement_estimate(job.work, core), system_.cores[core].id,
                                    core, static_cast<std::uint8_t>(thread));
        }
        if (candidates.empty()) return false;
        std::sort(candidates.begin(), candidates.end());
        for (const auto& candidate : candidates) {
            const auto core = std::get<2>(candidate);
            const auto thread = std::get<3>(candidate);
            auto trial = *model_;
            if (!trial.start(job.work, core, thread)) continue;
            auto after = trial.forecast();
            if (std::any_of(after.begin(), after.end(), [](const mvp::Completion& item) { return !item.on_time; })) {
                continue;
            }
            double wait_finish = 0;
            double trial_finish = 0;
            bool core_was_busy = false;
            for (const auto& item : predicted_) {
                const auto& running = jobs_[item.token];
                if (running.state == State::running && running.running_core == core) {
                    core_was_busy = true;
                    wait_finish = std::max(wait_finish, item.remaining_ns(time));
                }
            }
            for (const auto& item : after) {
                if (item.token == job.work.token || jobs_[item.token].running_core == core) {
                    trial_finish = std::max(trial_finish, item.remaining_ns(time));
                }
            }
            if (core_was_busy && trial_finish > wait_finish + model_->solo_duration(job.work, core)) continue;
            *model_ = std::move(trial);
            predicted_ = std::move(after);
            job.state = State::running;
            job.running_core = core;
            decisions.push_back({job.set_id, job.id, system_.cores[core].id,
                                 static_cast<std::uint8_t>(thread + 1), time});
            return true;
        }
        return false;
    }
};
}

std::unique_ptr<IScheduler> create_scheduler() { return std::make_unique<Scheduler>(); }

}
