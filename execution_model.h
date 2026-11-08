#ifndef ARENA_EXECUTION_MODEL_H
#define ARENA_EXECUTION_MODEL_H

#include "spec_system.h"
#include <array>
#include <cstddef>
#include <limits>

namespace arena::mvp {

enum class Phase { load, compute, write };

struct Work {
    std::size_t token;
    std::uint8_t type;
    std::uint8_t dram_id;
    std::uint32_t load_kb;
    std::uint32_t write_kb;
    std::uint32_t compute_units;
    std::uint64_t deadline_ns;
};

struct Attempt {
    Work work;
    std::size_t core_index;
    std::uint8_t thread_id;
    Phase phase;
    double remaining;
};

struct Completion {
    std::size_t token;
    std::uint64_t origin_ns;
    double elapsed_ns;
    bool on_time;

    std::uint64_t rounded_time_ns() const;
    double remaining_ns(std::uint64_t time_ns) const;
    double approximate_time_ns() const;
};

constexpr std::uint64_t no_wakeup = std::numeric_limits<std::uint64_t>::max();
std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b);
std::uint64_t rounded_time_after(std::uint64_t origin_ns, double elapsed_ns);
std::uint64_t request_after(std::uint64_t event_ns, std::uint64_t now);

class ExecutionModel {
public:
    explicit ExecutionModel(const SystemSpec& system);

    const SystemSpec& system() const { return *system_; }
    std::uint64_t now() const { return now_ns_; }
    const std::vector<Attempt>& active() const { return active_; }
    bool compatible(const Work& work, std::size_t core) const;
    std::uint32_t reserved_kb(std::size_t core) const;
    int free_thread(std::size_t core) const;
    double solo_duration(const Work& work, std::size_t core) const;
    double placement_estimate(const Work& work, std::size_t core) const;
    bool start(const Work& work, std::size_t core, std::uint8_t thread);
    bool preempt(std::size_t core, std::uint8_t thread);
    std::vector<Completion> advance_to(std::uint64_t target_ns);
    std::vector<Completion> forecast() const;
    std::uint64_t next_event_time() const;

private:
    const SystemSpec* system_;
    std::array<DRAMSpec, 256> drams_{};
    std::array<bool, 256> dram_exists_{};
    std::shared_ptr<std::map<std::uint64_t, double>> symbiosis_cache_;
    std::uint64_t now_ns_ = 0;
    std::vector<Attempt> active_;

    std::vector<Completion> advance_for(double duration_ns);
    double stretch(std::size_t core, std::array<std::uint8_t, 4> types, std::size_t count) const;
    std::array<double, 128> rates() const;
    bool next_phase(Attempt& attempt);
};

}
#endif
