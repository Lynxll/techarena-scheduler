





#include "spec_scheduler.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using Real = long double;
using Key = std::pair<std::uint16_t, std::uint16_t>;
constexpr auto never = std::numeric_limits<std::uint64_t>::max();


constexpr Real tolerance_ns = 1e-7L;
constexpr std::size_t event_limit = 10000;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct ConstantSymbiosis final : arena::SymbiosisSpec {
    explicit ConstantSymbiosis(double coefficient) : value(coefficient) {}
    double get_symbiosis(std::uint8_t, const std::multiset<std::uint8_t>&) const override {
        return value;
    }
    double value;
};

struct JobData {
    arena::JobSpec job;
    std::uint8_t dram;
    std::uint64_t arrival;
};

struct Attempt {
    Key key;
    std::size_t core;
    std::uint8_t thread;
    unsigned phase = 0; 
    Real fraction_left = 1;
};

class Reference {
public:
    Reference(arena::SystemSpec system, const std::vector<arena::JobSetSpec>& sets)
        : hardware(std::move(system)) {
        for (const auto& set : sets) {
            for (const auto& job : set.jobs) {
                inputs.emplace(Key{set.id, job.id}, JobData{job, set.dram_id, set.arrival_ns});
            }
        }
    }

    void advance(Real target) {
        require(target >= now, "Reference time moved backwards");
        for (std::size_t event = 0; !running.empty(); ++event) {
            require(event < event_limit, "Reference event bound exceeded");
            std::vector<Real> durations;
            Real delta = std::numeric_limits<Real>::infinity();
            for (const auto& attempt : running) {
                durations.push_back(phase_duration(attempt));
                delta = std::min(delta, attempt.fraction_left * durations.back());
            }
            const Real available = target - now;
            const bool hits_event = delta <= available;
            const Real elapsed = hits_event ? delta : available;
            std::vector<bool> completed_phase;
            for (std::size_t i = 0; i < running.size(); ++i) {
                completed_phase.push_back(hits_event && running[i].fraction_left * durations[i] <= delta);
                if (durations[i] != 0) {
                    running[i].fraction_left = std::max(0.L, running[i].fraction_left - elapsed / durations[i]);
                }
            }
            now += elapsed;
            for (std::size_t i = running.size(); i-- > 0;) {
                if (!completed_phase[i]) continue;
                auto& attempt = running[i];
                ++attempt.phase;
                attempt.fraction_left = 1;
                if (attempt.phase == 2 && inputs.at(attempt.key).job.write_size_kb == 0) ++attempt.phase;
                if (attempt.phase == 3) {
                    finishes[attempt.key] = now;
                    running.erase(running.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }
            if (!hits_event) break;
        }
        now = target;
        
        
        for (std::size_t i = running.size(); i-- > 0;) {
            const auto& attempt = running[i];
            const bool final_phase = attempt.phase == 2 ||
                (attempt.phase == 1 && inputs.at(attempt.key).job.write_size_kb == 0);
            const Real residual = attempt.fraction_left * phase_duration(attempt);
            if (final_phase && residual <= tolerance_ns) {
                maximum_dust = std::max(maximum_dust, residual);
                finishes[attempt.key] = now + residual;
                running.erase(running.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }

    void start(const arena::SchedulerDecision& decision) {
        advance(static_cast<Real>(decision.start_time_ns));
        const Key key{decision.job_set_id, decision.job_id};
        require(inputs.count(key) != 0, "Unknown job");
        const auto& input = inputs.at(key);
        require(now >= input.arrival, "Early arrival");
        require(launched.insert(key).second, "Duplicate job attempt");
        for (const auto predecessor : input.job.deps) {
            const Key parent{key.first, predecessor};
            require(finishes.count(parent) != 0 && finishes.at(parent) <= now + tolerance_ns,
                    "Dependency started before independent completion: set=" + std::to_string(key.first) +
                    " job=" + std::to_string(key.second));
        }
        const auto found = std::find_if(hardware.cores.begin(), hardware.cores.end(), [&](const arena::CoreSpec& core) {
            return core.id == decision.core_id;
        });
        require(found != hardware.cores.end(), "Unknown core");
        const auto index = static_cast<std::size_t>(found - hardware.cores.begin());
        require(decision.thread_id >= 1 && decision.thread_id <= found->threads,
                "External thread id outside 1..threads");
        require(std::find(found->allowed_types.begin(), found->allowed_types.end(), input.job.type) !=
                    found->allowed_types.end(), "Incompatible type");
        require(std::find(found->dram_ids.begin(), found->dram_ids.end(), input.dram) != found->dram_ids.end(),
                "Incompatible DRAM");
        std::uint64_t reserved = input.job.load_size_kb + input.job.write_size_kb;
        for (const auto& attempt : running) {
            if (attempt.core != index) continue;
            require(attempt.thread != decision.thread_id,
                    "Thread reused before independent completion: set=" + std::to_string(key.first) +
                    " job=" + std::to_string(key.second));
            const auto& prior = inputs.at(attempt.key).job;
            reserved += prior.load_size_kb + prior.write_size_kb;
        }
        require(reserved <= found->sram_capacity_kb, "SRAM overflow");
        running.push_back({key, index, decision.thread_id, 0, 1});
    }

    Real phase_duration(const Attempt& selected) const {
        const auto& input = inputs.at(selected.key);
        const auto& core = hardware.cores.at(selected.core);
        unsigned peers_on_core = 0;
        unsigned peers_on_dram = 0;
        std::multiset<std::uint8_t> computing_types;
        for (const auto& attempt : running) {
            if (attempt.phase != selected.phase) continue;
            if (attempt.core == selected.core) {
                ++peers_on_core;
                computing_types.insert(inputs.at(attempt.key).job.type);
            }
            if (inputs.at(attempt.key).dram == input.dram) ++peers_on_dram;
        }
        if (selected.phase == 1) {
            const Real s = peers_on_core <= 1 ? 1 : hardware.symbiosis_repo->get_symbiosis(core.id, computing_types);
            return 1000.L * input.job.compute_units / static_cast<Real>(core.computing_power) *
                (static_cast<Real>(peers_on_core) - (peers_on_core - 1) * s);
        }
        const auto dram = std::find_if(hardware.dram_units.begin(), hardware.dram_units.end(), [&](const arena::DRAMSpec& item) {
            return item.id == input.dram;
        });
        require(dram != hardware.dram_units.end(), "Missing DRAM");
        const bool load = selected.phase == 0;
        const auto kb = load ? input.job.load_size_kb : input.job.write_size_kb;
        const Real chunk_count = std::ceil(static_cast<Real>(kb) / core.sram_chunk_size_kb);
        const Real local_cost = load ? core.mc_write_cost_to_sram : core.mc_read_cost_to_dram;
        const Real remote_cost = load ? core.mc_read_cost_to_sram : core.mc_write_cost_to_dram;
        const Real alpha = load ? core.sram_write_alpha : core.sram_read_alpha;
        const Real beta = load ? dram->read_beta : dram->write_beta;
        const Real serialized_local = local_cost * (1 + (peers_on_core - 1) * alpha);
        const Real serialized_remote = remote_cost * (1 + (peers_on_dram - 1) * beta);
        return 1000.L * chunk_count * std::max(serialized_local, serialized_remote) / static_cast<Real>(core.mc_power);
    }

    std::map<Key, JobData> inputs;
    std::map<Key, Real> finishes;
    std::set<Key> launched;
    Real maximum_dust = 0;
private:
    arena::SystemSpec hardware;
    std::vector<Attempt> running;
    Real now = 0;
};

arena::SystemSpec system(unsigned seed, unsigned core_count, unsigned threads, double symbiosis) {
    arena::SystemSpec result{};
    result.dram_units = {{11, 0.13 + (seed % 5) * 0.17, 0.07 + (seed % 7) * 0.11}, {25, 0.29, 0.53}};
    result.scheduling_period = 100;
    result.scheduling_budget = 100;
    result.symbiosis_repo = std::make_shared<ConstantSymbiosis>(symbiosis);
    for (unsigned index = 0; index < core_count; ++index) {
        arena::CoreSpec c{};
        c.id = static_cast<std::uint8_t>(7 + 12 * index);
        c.threads = static_cast<std::uint8_t>(threads);
        c.computing_power = 1.137 + ((seed * 17 + index * 29) % 97);
        c.mc_power = 1.271 + ((seed * 11 + index * 31) % 97);
        c.sram_capacity_kb = 16384;
        c.sram_chunk_size_kb = 64 + (seed * 13 + index * 29) % 512;
        c.mc_read_cost_to_sram = 0.379 + (seed * 3) % 7;
        c.mc_write_cost_to_sram = 0.193 + (seed * 5) % 11;
        c.mc_read_cost_to_dram = 0.241 + (seed * 7) % 13;
        c.mc_write_cost_to_dram = 0.617 + (seed * 11) % 17;
        c.sram_read_alpha = 0.13 + (seed % 4) * 0.19;
        c.sram_write_alpha = 0.03 + (seed % 5) * 0.17;
        c.allowed_types = {0, 1};
        c.dram_ids = {11, 25};
        result.cores.push_back(c);
    }
    return result;
}

std::vector<arena::JobSetSpec> workload(unsigned seed) {
    std::vector<arena::JobSetSpec> sets;
    for (std::uint16_t set_id = 1; set_id <= 2; ++set_id) {
        arena::JobSetSpec set{set_id, set_id == 1 ? 0ULL : 3217ULL,
                            static_cast<std::uint8_t>((seed % 2 == 0 || set_id == 1) ? 11 : 25), {}};
        for (std::uint16_t id = 1; id <= 24; ++id) {
            arena::JobSpec job{};
            job.id = id;
            job.type = static_cast<std::uint8_t>(id % 2);
            job.load_size_kb = static_cast<std::uint16_t>(1 + (seed * 13 + id * 31) % 2048);
            job.write_size_kb = static_cast<std::uint16_t>((seed * 19 + id * 37) % 2048);
            job.compute_units = 1 + (seed * 23 + id * 41) % 10000;
            job.priority = static_cast<std::uint8_t>((id * 3) % 101);
            job.relative_deadline_ns = 1'000'000'000;
            if (id > 4) job.deps.push_back(static_cast<std::uint16_t>(id - 4));
            set.jobs.push_back(job);
        }
        sets.push_back(set);
    }
    return sets;
}

std::pair<std::size_t, Real> run_case(unsigned seed, unsigned cores, unsigned threads, double symbiosis) {
    const auto hardware = system(seed, cores, threads, symbiosis);
    const auto sets = workload(seed);
    
    Reference reference(hardware, sets);
    auto scheduler = arena::create_scheduler();
    scheduler->set_system_spec(hardware);
    std::size_t next_arrival = 0;
    std::uint64_t time = 0;
    std::uint64_t next_periodic = 0;
    std::uint64_t requested = never;
    unsigned used_budget = 0;
    std::size_t callback_count = 0;
    for (std::size_t event = 0; event < event_limit; ++event) {
        reference.advance(static_cast<Real>(time));
        if (reference.finishes.size() == reference.inputs.size()) break;
        const bool periodic = time == next_periodic;
        const bool extra = time == requested;
        if (extra) requested = never;
        if (periodic) {
            next_periodic = time + hardware.scheduling_period * 1000;
            used_budget = 0;
        } else if (extra) ++used_budget;
        const auto consider = [&](std::uint64_t wakeup) {
            if (used_budget < hardware.scheduling_budget && wakeup > time && wakeup < next_periodic)
                requested = std::min(requested, wakeup);
        };
        std::vector<arena::JobSetSpec> batch;
        while (next_arrival < sets.size() && sets[next_arrival].arrival_ns == time)
            batch.push_back(sets[next_arrival++]);
        if (!batch.empty()) consider(scheduler->arrive_at(time, batch));
        if (periodic || extra) {
            ++callback_count;
            auto response = scheduler->commit_until(time);
            consider(std::get<1>(response));
            for (const auto& decision : std::get<0>(response)) {
                require(decision.start_time_ns == time, "Unexpected future or past start");
                reference.start(decision);
            }
        }
        const auto arrival = next_arrival == sets.size() ? never : sets[next_arrival].arrival_ns;
        const auto next_time = std::min({next_periodic, requested, arrival});
        require(next_time > time, "Callback loop did not advance");
        time = next_time;
    }
    scheduler->terminate();
    require(reference.finishes.size() == reference.inputs.size(), "Not all independently replayed jobs finished within event bound");
    for (const auto& item : reference.finishes) {
        const auto& input = reference.inputs.at(item.first);
        require(item.second < static_cast<Real>(input.arrival) + input.job.relative_deadline_ns, "Independent missed deadline");
    }
    return {callback_count, reference.maximum_dust};
}

void check_published_examples() {
    auto hardware = system(1, 1, 2, 0.5);
    auto& c = hardware.cores.front();
    c.computing_power = 5;
    c.mc_power = 4;
    c.sram_chunk_size_kb = 128;
    c.mc_read_cost_to_sram = 2;
    c.mc_write_cost_to_sram = 0.1;
    c.mc_read_cost_to_dram = 0.05;
    c.mc_write_cost_to_dram = 3;
    c.sram_read_alpha = 0.05;
    c.sram_write_alpha = 0.1;
    hardware.dram_units = {{11, 0.5, 0.6}, {25, 0.5, 0.6}};
    const arena::JobSpec first{1, 0, 250, 700, 15, 0, 1000000, {}};
    const arena::JobSpec second{1, 1, 1024, 70, 5, 0, 1000000, {}};
    for (const bool shared : {false, true}) {
        Reference reference(hardware, {{1, 0, 11, {first}},
                                      {2, 0, static_cast<std::uint8_t>(shared ? 11 : 25), {second}}});
        reference.start({1, 1, c.id, 1, 0});
        reference.start({2, 1, c.id, 2, 0});
        reference.advance(1000000);
        require(std::abs(reference.finishes.at({1, 1}) - (shared ? 9450.L : 8500.L)) < 1e-8L,
                "Independent reference failed public first-job numerical example");
        require(std::abs(reference.finishes.at({2, 1}) - (shared ? 6700.L : 5750.L)) < 1e-8L,
                "Independent reference failed public second-job numerical example");
    }
}
}

int main() {
    try {
        check_published_examples();
    } catch (const std::exception& error) {
        std::cerr << "FAIL published numerical examples: " << error.what() << '\n';
        return 1;
    }
    std::size_t cases = 0, callbacks = 0;
    Real dust = 0;
    const std::vector<double> symbioses{-1.0, -0.17, 0.0, 0.37, 0.83, 1.0};
    for (unsigned seed = 1; seed <= 30; ++seed) {
        for (unsigned cores = 1; cores <= 2; ++cores) {
            for (unsigned threads = 1; threads <= 2; ++threads) {
                const auto coefficient = symbioses[(seed + cores + threads) % symbioses.size()];
                try {
                    const auto result = run_case(seed, cores, threads, coefficient);
                    callbacks += result.first;
                    dust = std::max(dust, result.second);
                    ++cases;
                } catch (const std::exception& error) {
                    std::cerr << "FAIL synthetic seed=" << seed << " cores=" << cores
                              << " threads=" << threads << " symbiosis=" << coefficient
                              << ": " << error.what() << '\n';
                    return 1;
                }
            }
        }
    }
    std::cout << "Independent long-double reference PASS: published_expected_finishes=4 cases=" << cases
              << " jobs=" << cases * 48 << " callbacks=" << callbacks
              << " max_absorbed_final_phase_dust_ns=" << std::setprecision(18) << dust
              << " tolerance_ns=" << tolerance_ns << '\n';
}
