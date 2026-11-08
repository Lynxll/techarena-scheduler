#include "execution_model.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace arena::mvp {
namespace {
constexpr double ns_per_us = 1000.0;
double chunks(std::uint32_t kb, std::uint32_t chunk_kb) {
    return static_cast<double>((kb + chunk_kb - 1) / chunk_kb);
}
bool positive(double value) { return std::isfinite(value) && value > 0; }
bool coefficient(double value) { return std::isfinite(value) && value >= 0 && value <= 1; }
}

std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) {
    return b > no_wakeup - a ? no_wakeup : a + b;
}

std::uint64_t rounded_time_after(std::uint64_t origin_ns, double elapsed_ns) {
    if (std::isnan(elapsed_ns) || elapsed_ns < 0) throw std::invalid_argument("Invalid duration");
    if (!std::isfinite(elapsed_ns) || elapsed_ns >= static_cast<double>(no_wakeup)) {
        return no_wakeup;
    }
    return saturating_add(origin_ns, static_cast<std::uint64_t>(std::ceil(elapsed_ns)));
}

std::uint64_t request_after(std::uint64_t event_ns, std::uint64_t now) {
    return std::max(saturating_add(now, 1), event_ns);
}

std::uint64_t Completion::rounded_time_ns() const {
    return rounded_time_after(origin_ns, elapsed_ns);
}

double Completion::remaining_ns(std::uint64_t time_ns) const {
    if (time_ns < origin_ns) return static_cast<double>(origin_ns - time_ns) + elapsed_ns;
    return std::max(0.0, elapsed_ns - static_cast<double>(time_ns - origin_ns));
}

double Completion::approximate_time_ns() const {
    return static_cast<double>(origin_ns) + elapsed_ns;
}

ExecutionModel::ExecutionModel(const SystemSpec& system)
    : system_(&system), symbiosis_cache_(std::make_shared<std::map<std::uint64_t, double>>()) {
    if (system.cores.empty() || system.cores.size() > 32 || system.dram_units.size() > 4) {
        throw std::invalid_argument("Unsupported hardware size");
    }
    std::set<std::uint8_t> ids;
    for (const auto& core : system.cores) {
        if (!ids.insert(core.id).second || core.threads == 0 || core.threads > 4 ||
            core.sram_chunk_size_kb == 0 || !positive(core.computing_power) ||
            !positive(core.mc_power) || !positive(core.mc_read_cost_to_sram) ||
            !positive(core.mc_write_cost_to_sram) || !positive(core.mc_read_cost_to_dram) ||
            !positive(core.mc_write_cost_to_dram) || !coefficient(core.sram_read_alpha) ||
            !coefficient(core.sram_write_alpha)) {
            throw std::invalid_argument("Invalid core parameters");
        }
    }
    for (const auto& dram : system.dram_units) {
        if (dram_exists_[dram.id] || !coefficient(dram.read_beta) || !coefficient(dram.write_beta)) {
            throw std::invalid_argument("Invalid DRAM parameters");
        }
        drams_[dram.id] = dram;
        dram_exists_[dram.id] = true;
    }
    active_.reserve(128);
}

bool ExecutionModel::compatible(const Work& work, std::size_t index) const {
    if (index >= system_->cores.size() || !dram_exists_[work.dram_id]) return false;
    const auto& core = system_->cores[index];
    return work.load_kb + work.write_kb <= core.sram_capacity_kb &&
        std::find(core.allowed_types.begin(), core.allowed_types.end(), work.type) != core.allowed_types.end() &&
        std::find(core.dram_ids.begin(), core.dram_ids.end(), work.dram_id) != core.dram_ids.end();
}

std::uint32_t ExecutionModel::reserved_kb(std::size_t core) const {
    std::uint32_t result = 0;
    for (const auto& attempt : active_) {
        if (attempt.core_index == core) result += attempt.work.load_kb + attempt.work.write_kb;
    }
    return result;
}

int ExecutionModel::free_thread(std::size_t core) const {
    if (core >= system_->cores.size()) return -1;
    std::array<bool, 4> busy{};
    for (const auto& attempt : active_) {
        if (attempt.core_index == core) busy[attempt.thread_id] = true;
    }
    for (std::uint8_t thread = 0; thread < system_->cores[core].threads; ++thread) {
        if (!busy[thread]) return thread;
    }
    return -1;
}

double ExecutionModel::solo_duration(const Work& work, std::size_t index) const {
    const auto& core = system_->cores.at(index);
    const double load = chunks(work.load_kb, core.sram_chunk_size_kb) *
        std::max(core.mc_read_cost_to_sram, core.mc_write_cost_to_sram) / core.mc_power;
    const double write = chunks(work.write_kb, core.sram_chunk_size_kb) *
        std::max(core.mc_read_cost_to_dram, core.mc_write_cost_to_dram) / core.mc_power;
    return ns_per_us * (load + static_cast<double>(work.compute_units) / core.computing_power + write);
}

double ExecutionModel::stretch(std::size_t index, std::array<std::uint8_t, 4> types, std::size_t size) const {
    if (size <= 1) return 1;
    for (std::size_t i = 1; i < size; ++i) {
        for (std::size_t j = i; j > 0 && types[j] < types[j - 1]; --j) std::swap(types[j], types[j - 1]);
    }
    std::uint64_t key = system_->cores[index].id;
    key |= static_cast<std::uint64_t>(size) << 8U;
    for (std::size_t i = 0; i < size; ++i) {
        key |= static_cast<std::uint64_t>(types[i]) << (16U + 8U * i);
    }
    const auto found = symbiosis_cache_->find(key);
    if (found != symbiosis_cache_->end()) return found->second;
    const double symbiosis = system_->symbiosis_repo
        ? system_->symbiosis_repo->get_symbiosis(system_->cores[index].id,
                                              std::multiset<std::uint8_t>(types.begin(), types.begin() + static_cast<std::ptrdiff_t>(size)))
        : 0.0;
    if (!std::isfinite(symbiosis) || symbiosis < -1 || symbiosis > 1) {
        throw std::invalid_argument("Invalid symbiosis coefficient");
    }
    const double count = static_cast<double>(size);
    const double factor = count - (count - 1) * symbiosis;
    symbiosis_cache_->emplace(key, factor);
    return factor;
}

double ExecutionModel::placement_estimate(const Work& work, std::size_t index) const {
    const auto& core = system_->cores[index];
    std::array<std::uint8_t, 4> types{work.type};
    std::size_t type_count = 1;
    std::size_t dram_residents = 0;
    for (const auto& attempt : active_) {
        if (attempt.core_index == index) {
            if (type_count == types.size()) return std::numeric_limits<double>::infinity();
            types[type_count++] = attempt.work.type;
        }
        if (attempt.work.dram_id == work.dram_id) ++dram_residents;
    }
    const double peers = static_cast<double>(type_count - 1);
    const double dram_peers = static_cast<double>(dram_residents);
    const auto& dram = drams_[work.dram_id];
    const double load = chunks(work.load_kb, core.sram_chunk_size_kb) / core.mc_power *
        std::max(core.mc_read_cost_to_sram * (1 + dram_peers * dram.read_beta),
                 core.mc_write_cost_to_sram * (1 + peers * core.sram_write_alpha));
    const double write = chunks(work.write_kb, core.sram_chunk_size_kb) / core.mc_power *
        std::max(core.mc_read_cost_to_dram * (1 + peers * core.sram_read_alpha),
                 core.mc_write_cost_to_dram * (1 + dram_peers * dram.write_beta));
    return ns_per_us * (load + write + static_cast<double>(work.compute_units) /
                       core.computing_power * stretch(index, types, type_count));
}

bool ExecutionModel::start(const Work& work, std::size_t core, std::uint8_t thread) {
    if (!compatible(work, core) || thread >= system_->cores[core].threads ||
        reserved_kb(core) + work.load_kb + work.write_kb > system_->cores[core].sram_capacity_kb) {
        return false;
    }
    for (const auto& attempt : active_) {
        if (attempt.work.token == work.token ||
            (attempt.core_index == core && attempt.thread_id == thread)) return false;
    }
    Attempt attempt{work, core, thread, Phase::load,
                    chunks(work.load_kb, system_->cores[core].sram_chunk_size_kb)};
    if (attempt.remaining == 0) {
        attempt.phase = Phase::compute;
        attempt.remaining = work.compute_units;
    }
    active_.push_back(attempt);
    return true;
}

bool ExecutionModel::preempt(std::size_t core, std::uint8_t thread) {
    const auto it = std::find_if(active_.begin(), active_.end(), [core, thread](const Attempt& attempt) {
        return attempt.core_index == core && attempt.thread_id == thread;
    });
    if (it == active_.end()) return false;
    active_.erase(it);
    return true;
}

std::array<double, 128> ExecutionModel::rates() const {
    std::array<unsigned, 32> loading{}, writing{};
    std::array<unsigned, 256> dram_readers{}, dram_writers{};
    std::array<std::array<std::uint8_t, 4>, 32> computing{};
    std::array<std::size_t, 32> compute_counts{};
    for (const auto& attempt : active_) {
        if (attempt.phase == Phase::load) {
            ++loading[attempt.core_index];
            ++dram_readers[attempt.work.dram_id];
        } else if (attempt.phase == Phase::write) {
            ++writing[attempt.core_index];
            ++dram_writers[attempt.work.dram_id];
        } else {
            computing[attempt.core_index][compute_counts[attempt.core_index]++] = attempt.work.type;
        }
    }
    std::array<double, 32> compute_stretch{};
    for (std::size_t core = 0; core < system_->cores.size(); ++core) {
        compute_stretch[core] = stretch(core, computing[core], compute_counts[core]);
    }
    std::array<double, 128> result{};
    std::size_t rate_index = 0;
    for (const auto& attempt : active_) {
        const auto index = attempt.core_index;
        const auto& core = system_->cores[index];
        const auto& dram = drams_[attempt.work.dram_id];
        if (attempt.phase == Phase::compute) {
            result[rate_index++] = core.computing_power / (ns_per_us * compute_stretch[index]);
        } else {
            double cost;
            if (attempt.phase == Phase::load) {
                cost = std::max(core.mc_read_cost_to_sram *
                                   (1 + (dram_readers[dram.id] - 1) * dram.read_beta),
                               core.mc_write_cost_to_sram *
                                   (1 + (loading[index] - 1) * core.sram_write_alpha));
            } else {
                cost = std::max(core.mc_read_cost_to_dram *
                                   (1 + (writing[index] - 1) * core.sram_read_alpha),
                               core.mc_write_cost_to_dram *
                                   (1 + (dram_writers[dram.id] - 1) * dram.write_beta));
            }
            result[rate_index++] = core.mc_power / (ns_per_us * cost);
        }
    }
    return result;
}

bool ExecutionModel::next_phase(Attempt& attempt) {
    if (attempt.phase == Phase::load) {
        attempt.phase = Phase::compute;
        attempt.remaining = attempt.work.compute_units;
        if (attempt.remaining > 0) return true;
    }
    if (attempt.phase == Phase::compute) {
        attempt.phase = Phase::write;
        attempt.remaining = chunks(attempt.work.write_kb,
                                   system_->cores[attempt.core_index].sram_chunk_size_kb);
        if (attempt.remaining > 0) return true;
    }
    return false;
}

std::vector<Completion> ExecutionModel::advance_to(std::uint64_t target_ns) {
    if (target_ns < now_ns_) throw std::invalid_argument("Time moved backwards");
    auto completed = advance_for(static_cast<double>(target_ns - now_ns_));
    now_ns_ = target_ns;
    return completed;
}

std::vector<Completion> ExecutionModel::advance_for(double duration_ns) {
    std::vector<Completion> completed;
    double elapsed_ns = 0;
    while (!active_.empty()) {
        const auto speeds = rates();
        std::array<double, 128> durations{};
        double delta = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < active_.size(); ++i) {
            durations[i] = active_[i].remaining / speeds[i];
            delta = std::min(delta, durations[i]);
        }
        if (elapsed_ns + delta > duration_ns) {
            const double elapsed = duration_ns - elapsed_ns;
            for (std::size_t i = 0; i < active_.size(); ++i) {
                active_[i].remaining = std::max(0.0, active_[i].remaining - elapsed * speeds[i]);
            }
            return completed;
        }
        elapsed_ns += delta;
        for (std::size_t i = active_.size(); i-- > 0;) {
            auto& attempt = active_[i];
            attempt.remaining = std::max(0.0, attempt.remaining - delta * speeds[i]);
            if (durations[i] <= delta && !next_phase(attempt)) {
                completed.push_back({attempt.work.token, now_ns_, elapsed_ns,
                                     attempt.work.deadline_ns > now_ns_ &&
                                     elapsed_ns < static_cast<double>(attempt.work.deadline_ns - now_ns_)});
                active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }
    return completed;
}

std::vector<Completion> ExecutionModel::forecast() const {
    auto copy = *this;
    return copy.advance_for(std::numeric_limits<double>::infinity());
}

std::uint64_t ExecutionModel::next_event_time() const {
    const auto speeds = rates();
    double result = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < active_.size(); ++i) {
        result = std::min(result, active_[i].remaining / speeds[i]);
    }
    return rounded_time_after(now_ns_, result);
}

}
