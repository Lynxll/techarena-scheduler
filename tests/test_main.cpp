#include "execution_model.h"
#include "spec_scheduler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace arena;
using namespace arena::mvp;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected) {
    if (std::abs(actual - expected) > 0.0001) {
        throw std::runtime_error("Expected " + std::to_string(expected) + ", got " + std::to_string(actual));
    }
}

class Symbiosis final : public SymbiosisSpec {
public:
    explicit Symbiosis(double value) : value_(value) {}
    double get_symbiosis(std::uint8_t, const std::multiset<std::uint8_t>&) const override { return value_; }
private:
    double value_;
};

CoreSpec core(std::uint8_t id = 7, std::uint8_t threads = 2) {
    CoreSpec result{};
    result.id = id;
    result.computing_power = 5;
    result.threads = threads;
    result.sram_capacity_kb = 2048;
    result.sram_chunk_size_kb = 128;
    result.mc_power = 4;
    result.mc_read_cost_to_sram = 2;
    result.mc_write_cost_to_sram = 0.1;
    result.mc_read_cost_to_dram = 0.05;
    result.mc_write_cost_to_dram = 3;
    result.sram_read_alpha = 0.05;
    result.sram_write_alpha = 0.1;
    result.allowed_types = {0, 1, 2};
    result.dram_ids = {11, 25};
    return result;
}

SystemSpec system(double symbiosis = 1) {
    SystemSpec result{};
    result.cores = {core()};
    result.dram_units = {{11, 0.5, 0.6}, {25, 0.5, 0.6}};
    result.scheduling_period = 100;
    result.scheduling_budget = 100;
    result.symbiosis_repo = std::make_shared<Symbiosis>(symbiosis);
    return result;
}

Work work(std::size_t token, std::uint32_t load = 250, std::uint32_t write = 700,
          std::uint32_t compute = 15, std::uint8_t dram = 11) {
    return {token, 0, dram, load, write, compute, 1'000'000'000};
}

double finish(const std::vector<Completion>& items, std::size_t token) {
    for (const auto& item : items) if (item.token == token) return item.approximate_time_ns();
    throw std::runtime_error("Missing completion");
}

JobSpec job(std::uint16_t id, std::uint32_t units = 5, std::uint64_t deadline = 1'000'000) {
    JobSpec result{};
    result.id = id;
    result.type = 0;
    result.load_size_kb = 128;
    result.compute_units = units;
    result.priority = 50;
    result.relative_deadline_ns = deadline;
    return result;
}

void test_pdf_memory_examples() {
    auto spec = system();
    ExecutionModel solo(spec);
    near(solo.solo_duration(work(1), 0), 8500);
    check(solo.start(work(1), 0, 0), "Solo launch");
    near(finish(solo.forecast(), 1), 8500);
    for (bool shared : {false, true}) {
        ExecutionModel model(spec);
        check(model.start(work(1), 0, 0), "PDF first launch");
        check(model.start(work(2, 1024, 70, 5, shared ? 11 : 25), 0, 1), "PDF second launch");
        const auto result = model.forecast();
        near(finish(result, 1), shared ? 9450 : 8500);
        near(finish(result, 2), shared ? 6700 : 5750);
        check(model.active().size() == 2 && model.now() == 0, "Forecast mutated live state");
    }
}

void test_pdf_symbiosis_example() {
    auto spec = system(0.3);
    auto& c = spec.cores[0];
    c.computing_power = c.mc_power = 1;
    c.sram_chunk_size_kb = 64;
    c.mc_read_cost_to_sram = c.mc_write_cost_to_sram = 10;
    c.mc_read_cost_to_dram = c.mc_write_cost_to_dram = 10;
    c.sram_read_alpha = c.sram_write_alpha = 0;
    for (auto& dram : spec.dram_units) dram.read_beta = dram.write_beta = 0;
    ExecutionModel model(spec);
    check(model.start(work(1, 64, 64, 20), 0, 0), "First symbiosis job");
    model.advance_to(10000);
    check(model.start(work(2, 64, 64, 20), 0, 1), "Second symbiosis job");
    const auto result = model.forecast();
    near(finish(result, 1), 47000);
    near(finish(result, 2), 57000);
}

void test_symbiosis_extremes_and_multisets() {
    for (double s : {-1.0, 0.0, 1.0}) {
        for (std::uint8_t count : std::array<std::uint8_t, 4>{1, 2, 3, 4}) {
            auto spec = system(s);
            spec.cores[0].threads = 4;
            spec.cores[0].computing_power = 1;
            spec.cores[0].sram_write_alpha = 0;
            spec.dram_units[0].read_beta = 0;
            ExecutionModel model(spec);
            for (std::uint8_t i = 0; i < count; ++i) check(model.start(work(i, 128, 0, 20), 0, i), "SMT launch");
            const auto predicted = model.forecast();
            for (std::uint8_t i = 0; i < count; ++i) {
                near(finish(predicted, i), 500 + 20000 * (count - (count - 1) * s));
            }
        }
    }
}

void test_independent_read_write() {
    auto spec = system();
    auto& c = spec.cores[0];
    c.computing_power = c.mc_power = 1;
    c.mc_read_cost_to_sram = c.mc_write_cost_to_dram = 10;
    c.sram_read_alpha = c.sram_write_alpha = 1;
    spec.dram_units[0].read_beta = spec.dram_units[0].write_beta = 1;
    ExecutionModel model(spec);
    check(model.start(work(1, 128, 128, 1), 0, 0), "Load/write first");
    model.advance_to(11000);
    check(model.start(work(2, 128, 128, 1), 0, 1), "Load/write second");
    const auto predicted = model.forecast();
    near(finish(predicted, 1), 21000);
    near(finish(predicted, 2), 32000);
}

void test_shared_dram_different_cores() {
    auto spec = system();
    spec.cores.push_back(core(99, 1));
    spec.cores[0].computing_power = spec.cores[1].computing_power = 100;
    spec.cores[1].mc_power = 2;
    ExecutionModel model(spec);
    check(model.start(work(1, 128, 0, 1), 0, 0), "DRAM first");
    check(model.start(work(2, 128, 0, 1), 1, 0), "DRAM second");
    const auto predicted = model.forecast();
    near(finish(predicted, 1), 760);
    near(finish(predicted, 2), 1260);
}

void test_sram_dominant_and_admission() {
    auto spec = system();
    auto& c = spec.cores[0];
    c.mc_power = 1;
    c.computing_power = 100;
    c.mc_read_cost_to_sram = 0.1;
    c.mc_write_cost_to_sram = 2;
    c.sram_write_alpha = 1;
    c.sram_capacity_kb = 256;
    ExecutionModel model(spec);
    check(model.start(work(1, 128, 0, 1), 0, 0), "SRAM first");
    check(!model.start(work(2, 129, 0, 1), 0, 1), "SRAM overflow accepted");
    check(model.start(work(2, 128, 0, 1), 0, 1), "Exact SRAM fit rejected");
    check(!model.start(work(3, 1, 0, 1), 0, 0), "Occupied thread accepted");
    const auto predicted = model.forecast();
    near(finish(predicted, 1), 4010);
    near(finish(predicted, 2), 4010);
    check(model.reserved_kb(0) == 256, "SRAM reservation incorrect");
    model.advance_to(4010);
    check(model.reserved_kb(0) == 0, "SRAM not released");
    auto invalid = work(4);
    invalid.type = 250;
    check(!model.start(invalid, 0, 0), "Invalid type accepted");
    invalid = work(5, 1, 0, 1, 200);
    check(!model.start(invalid, 0, 0), "Invalid DRAM accepted");
    check(!model.start(work(6, 1, 0, 1), 0, 2), "Invalid thread accepted");
}

void test_restart_and_time_boundaries() {
    auto spec = system();
    ExecutionModel model(spec);
    check(model.start(work(1), 0, 0), "Restart initial");
    model.advance_to(2000);
    check(model.preempt(0, 0), "Preemption failed");
    check(model.reserved_kb(0) == 0 && model.active().empty(), "Preemption did not release resources");
    check(model.start(work(1), 0, 1), "Restart launch");
    near(finish(model.forecast(), 1), 10500);
    auto exact = work(2);
    exact.deadline_ns = 8500;
    ExecutionModel boundary(spec);
    check(boundary.start(exact, 0, 0), "Boundary start");
    check(!boundary.forecast()[0].on_time, "Strict deadline equality accepted");
    check(request_after(rounded_time_after(4, 0.1), 4) == 5, "Wakeup must round up");
    check(request_after(4, 4) == 5, "Wakeup must be in future");
    check(saturating_add(no_wakeup - 1, 10) == no_wakeup, "Deadline overflow");
    check(rounded_time_after(no_wakeup - 1, 1.1) == no_wakeup, "Fractional duration overflow");
    check(rounded_time_after(1, static_cast<double>(no_wakeup)) == no_wakeup, "Duration conversion overflow");
    check(request_after(rounded_time_after(4, std::numeric_limits<double>::infinity()), 4) == no_wakeup,
          "Idle sentinel");
}

void test_integer_time_anchor() {
    struct Example {
        unsigned computing_power;
        unsigned mc_power;
        double cost;
        std::uint32_t compute_units;
        std::uint64_t duration_numerator;
        std::uint64_t duration_denominator;
    };
    const std::array<Example, 2> examples{{
        {81, 97, 0.1, 49, 47'692'000, 78'570},
        {93, 37, 0.0002, 89, 32'930'372'000, 34'410'000},
    }};
    const std::array<std::uint64_t, 4> origins{
        0, 999'999'000'000ULL, (std::uint64_t{1} << 53U) + 17, no_wakeup - 5000};
    for (const auto& example : examples) {
        const auto rounded_duration = (example.duration_numerator + example.duration_denominator - 1) /
                                      example.duration_denominator;
        for (const auto origin : origins) {
            auto spec = system();
            auto& c = spec.cores[0];
            c.threads = 1;
            c.computing_power = example.computing_power;
            c.mc_power = example.mc_power;
            c.sram_capacity_kb = 128;
            c.sram_chunk_size_kb = 64;
            c.mc_read_cost_to_sram = c.mc_write_cost_to_sram = example.cost;
            c.mc_read_cost_to_dram = c.mc_write_cost_to_dram = example.cost;
            c.sram_read_alpha = c.sram_write_alpha = 0;
            spec.dram_units[0].read_beta = spec.dram_units[0].write_beta = 0;
            auto item = work(1, 64, 64, example.compute_units);
            item.deadline_ns = origin + rounded_duration;
            ExecutionModel model(spec);
            model.advance_to(origin);
            check(model.now() == origin, "Integer callback timestamp lost precision");
            check(model.start(item, 0, 0), "Integer-time model launch failed");
            const auto forecast = model.forecast().front();
            check(forecast.origin_ns == origin && forecast.rounded_time_ns() == origin + rounded_duration,
                  "Translated completion rounded to an earlier integer timestamp");
            near(forecast.elapsed_ns, static_cast<double>(example.duration_numerator) / example.duration_denominator);
            check(forecast.on_time, "Relative deadline comparison lost precision");
            check(forecast.remaining_ns(origin + rounded_duration - 1) > 0,
                  "Cached completion lost its fractional remaining duration");
            check(model.advance_to(origin + 1).empty(), "Job completed before its minimum compute duration");
            check(model.advance_to(origin + rounded_duration - 1).empty() && model.active().size() == 1,
                  "Model released the thread before the analytical completion");
            const auto done = model.advance_to(origin + rounded_duration);
            check(done.size() == 1 && done[0].on_time && model.active().empty(),
                  "Model did not finish at the first valid integer callback");
            check(done[0].rounded_time_ns() == forecast.rounded_time_ns(),
                  "Split advancement changed the rounded completion timestamp");

            auto first = job(1, example.compute_units, 4000);
            first.load_size_kb = first.write_size_kb = 64;
            auto second = first;
            second.id = 2;
            auto scheduler = create_scheduler();
            scheduler->set_system_spec(spec);
            scheduler->arrive_at(origin, {{1, origin, 11, {first, second}}});
            auto [decisions, wakeup] = scheduler->commit_until(origin);
            check(decisions.size() == 1 && decisions[0].job_id == 1 && wakeup == origin + rounded_duration,
                  "Scheduler wakeup lost relative completion precision");
            check(std::get<0>(scheduler->commit_until(origin + rounded_duration - 1)).empty(),
                  "Scheduler reused a thread before the analytical completion");
            const auto next = std::get<0>(scheduler->commit_until(origin + rounded_duration));
            check(next.size() == 1 && next[0].job_id == 2 && next[0].start_time_ns == origin + rounded_duration,
                  "Scheduler did not launch the next job at the valid integer callback");
            scheduler->terminate();
        }
    }
}

void test_latest_start_order_at_large_origins() {
    
    
    for (const auto origin : std::array<std::uint64_t, 2>{0, std::uint64_t{1} << 63U}) {
        auto spec = system();
        auto& c = spec.cores[0];
        c.threads = 1;
        c.computing_power = c.mc_power = 100;
        c.sram_chunk_size_kb = 64;
        c.sram_capacity_kb = 128;
        c.mc_read_cost_to_sram = c.mc_write_cost_to_sram = 0.0001;
        c.mc_read_cost_to_dram = c.mc_write_cost_to_dram = 0.0001;
        auto urgent = job(1, 32, 350), later = job(2, 32, 700);
        urgent.load_size_kb = later.load_size_kb = 1;
        urgent.priority = 100;
        later.priority = 0;
        auto scheduler = create_scheduler();
        scheduler->set_system_spec(spec);
        scheduler->arrive_at(origin, {{1, origin, 11, {later, urgent}}});
        ExecutionModel verifier(spec);
        verifier.advance_to(origin);
        const auto first = scheduler->commit_until(origin);
        check(std::get<0>(first).size() == 1 && std::get<0>(first)[0].job_id == urgent.id,
              "Large origin changed earliest latest-start ordering");
        check(std::get<1>(first) == origin + 321, "Wrong first completion wakeup");
        
        auto urgent_work = work(urgent.id, 1, 0, 32);
        urgent_work.deadline_ns = origin + urgent.relative_deadline_ns;
        check(verifier.start(urgent_work, 0, 0), "Ordering verifier urgent launch");
        check(std::get<0>(scheduler->commit_until(origin + 320)).empty(), "Urgent thread reused early");
        const auto urgent_done = verifier.advance_to(origin + 321);
        check(urgent_done.size() == 1 && urgent_done[0].on_time, "Urgent job missed its deadline");
        const auto next = std::get<0>(scheduler->commit_until(origin + 321));
        check(next.size() == 1 && next[0].job_id == later.id && next[0].start_time_ns == origin + 321,
              "Later job did not follow urgent job");
        auto later_work = work(later.id, 1, 0, 32);
        later_work.deadline_ns = origin + later.relative_deadline_ns;
        check(verifier.start(later_work, 0, 0), "Ordering verifier later launch");
        const auto later_done = verifier.advance_to(origin + 642);
        check(later_done.size() == 1 && later_done[0].on_time && verifier.active().empty(),
              "Translated earliest-start schedule did not complete both jobs");
        scheduler->terminate();

        auto parent = job(10, 32, 1000), child = job(20, 32, 700), unrelated = job(30, 32, 1050);
        parent.load_size_kb = child.load_size_kb = unrelated.load_size_kb = 1;
        parent.priority = child.priority = 100;
        unrelated.priority = 0;
        child.deps = {parent.id};
        scheduler = create_scheduler();
        scheduler->set_system_spec(spec);
        scheduler->arrive_at(origin, {{1, origin, 11, {unrelated, child, parent}}});
        const auto parent_start = std::get<0>(scheduler->commit_until(origin));
        check(parent_start.size() == 1 && parent_start[0].job_id == parent.id,
              "Large origin lost inherited child deadline");
        check(std::get<0>(scheduler->commit_until(origin + 320)).empty(), "Dependency released early");
        const auto child_start = std::get<0>(scheduler->commit_until(origin + 321));
        check(child_start.size() == 1 && child_start[0].job_id == child.id,
              "Dependency was not released at the first safe integer callback");
        check(std::get<0>(scheduler->commit_until(origin + 641)).empty(), "Child thread reused early");
        const auto unrelated_start = std::get<0>(scheduler->commit_until(origin + 642));
        check(unrelated_start.size() == 1 && unrelated_start[0].job_id == unrelated.id,
              "Inherited latest-start ordering did not preserve the full chain");
        check(std::get<0>(scheduler->commit_until(origin + 963)).empty(), "Completed chain relaunched work");
        scheduler->terminate();
    }
    auto spec = system();
    spec.cores[0].threads = 1;
    auto parent = job(1), incompatible_child = job(2), unrelated = job(3);
    incompatible_child.type = 255;
    incompatible_child.deps = {parent.id};
    auto scheduler = create_scheduler();
    scheduler->set_system_spec(spec);
    scheduler->arrive_at(0, {{1, 0, 11, {unrelated, incompatible_child, parent}}});
    const auto decisions = std::get<0>(scheduler->commit_until(0));
    check(decisions.size() == 1 && decisions[0].job_id == parent.id,
          "Infinite descendant duration destabilized the ready comparator");
    scheduler->terminate();
}

using Key = std::pair<std::uint16_t, std::uint16_t>;
struct ReplayResult {
    std::vector<SchedulerDecision> decisions;
    std::map<Key, Completion> finishes;
    std::size_t commits = 0;
    std::size_t expected = 0;
    bool passed() const { return finishes.size() == expected; }
};


ReplayResult replay(SystemSpec spec, std::vector<JobSetSpec> sets,
                    std::uint64_t period_ns = 100000, bool honor_requests = true) {
    std::stable_sort(sets.begin(), sets.end(), [](const JobSetSpec& a, const JobSetSpec& b) {
        return a.arrival_ns < b.arrival_ns;
    });
    ReplayResult result;
    std::map<Key, Work> works;
    std::map<Key, const JobSpec*> inputs;
    std::map<std::size_t, Key> token_keys;
    std::map<Key, std::uint64_t> arrivals;
    std::uint64_t horizon = period_ns;
    for (const auto& set : sets) {
        for (const auto& item : set.jobs) {
            const Key key{set.id, item.id};
            const auto deadline = saturating_add(set.arrival_ns, item.relative_deadline_ns);
            const auto token = result.expected++;
            works.emplace(key, Work{token, item.type, set.dram_id, item.load_size_kb,
                                     item.write_size_kb, item.compute_units, deadline});
            inputs[key] = &item;
            token_keys[token] = key;
            arrivals[key] = set.arrival_ns;
            horizon = std::max(horizon, saturating_add(deadline, period_ns));
        }
    }
    auto scheduler = create_scheduler();
    scheduler->set_system_spec(spec);
    ExecutionModel simulator(spec);
    std::set<Key> launched;
    std::size_t next_arrival = 0;
    std::uint64_t time = 0, guaranteed = 0, requested = no_wakeup, used = 0;
    for (std::size_t event = 0; event < 1'000'000 && time <= horizon; ++event) {
        for (const auto& completion : simulator.advance_to(time)) {
            if (completion.on_time) result.finishes[token_keys.at(completion.token)] = completion;
        }
        if (result.passed()) break;
        const bool periodic = time == guaranteed;
        const bool extra = time == requested;
        if (extra) requested = no_wakeup;
        if (periodic) {
            used = 0;
            guaranteed = saturating_add(time, period_ns);
        } else if (extra) {
            ++used;
        }
        auto consider = [&](std::uint64_t when) {
            if (honor_requests && used < spec.scheduling_budget && when > time && when < guaranteed) {
                requested = std::min(requested, when);
            }
        };
        std::vector<JobSetSpec> batch;
        while (next_arrival < sets.size() && sets[next_arrival].arrival_ns == time) {
            batch.push_back(sets[next_arrival++]);
        }
        if (!batch.empty()) consider(scheduler->arrive_at(time, batch));
        if (periodic || extra) {
            ++result.commits;
            auto [decisions, wakeup] = scheduler->commit_until(time);
            consider(wakeup);
            for (const auto& decision : decisions) {
                check(decision.start_time_ns == time, "Backdated or future decision");
                const Key key{decision.job_set_id, decision.job_id};
                check(works.count(key) == 1, "Unknown job in decision");
                check(arrivals.at(key) <= time, "Scheduled before arrival");
                check(launched.insert(key).second, "MVP must not restart jobs");
                for (auto parent : inputs.at(key)->deps) {
                    check(result.finishes.count({key.first, parent}) == 1, "Scheduled before predecessor write completed");
                }
                const auto found = std::find_if(spec.cores.begin(), spec.cores.end(), [&](const CoreSpec& c) {
                    return c.id == decision.core_id;
                });
                check(found != spec.cores.end(), "Unknown core id");
                const auto index = static_cast<std::size_t>(found - spec.cores.begin());
                check(decision.thread_id >= 1 && decision.thread_id <= found->threads,
                      "Public thread ID must be in 1..threads");
                const auto model_thread = static_cast<std::uint8_t>(decision.thread_id - 1);
                check(simulator.start(works.at(key), index, model_thread), "Invalid resource assignment");
                result.decisions.push_back(decision);
            }
        }
        const auto arrival = next_arrival < sets.size() ? sets[next_arrival].arrival_ns : no_wakeup;
        const auto next = std::min({guaranteed, requested, arrival});
        check(next > time, "Harness event loop stalled");
        time = next;
    }
    scheduler->terminate();
    return result;
}

void test_dag_arrivals_and_ids() {
    auto spec = system();
    auto child = job(10);
    child.deps = {90};
    auto parent = job(90);
    parent.write_size_kb = 700;
    auto r = replay(spec, {{500, 1000, 11, {child, parent}}, {42, 2100, 25, {job(10)}}});
    check(r.passed() && r.decisions.size() == 3, "Dynamic DAG replay failed");
    for (const auto& decision : r.decisions) {
        check(decision.core_id == 7, "Core id treated as index");
        if (decision.job_set_id == 500 && decision.job_id == 10) {
            check(decision.start_time_ns >= r.finishes.at(Key{std::uint16_t{500}, std::uint16_t{90}}).rounded_time_ns(),
                  "Dependency launched too early");
        }
    }
}

void test_public_thread_ids() {
    for (std::uint8_t count = 1; count <= 4; ++count) {
        auto spec = system(1);
        spec.cores[0].threads = count;
        spec.cores[0].sram_write_alpha = 0;
        spec.dram_units[0].read_beta = 0;
        JobSetSpec set{1, 0, 11, {}};
        for (std::uint16_t id = 1; id <= count; ++id) set.jobs.push_back(job(id, 100));
        auto scheduler = create_scheduler();
        scheduler->set_system_spec(spec);
        scheduler->arrive_at(0, {set});
        const auto decisions = std::get<0>(scheduler->commit_until(0));
        check(decisions.size() == count, "Scheduler did not use all available public thread slots");
        std::set<std::uint8_t> threads;
        for (const auto& decision : decisions) {
            check(decision.core_id == spec.cores[0].id, "Wrong core for thread contract check");
            check(decision.thread_id >= 1 && decision.thread_id <= count,
                  "Public API returned thread outside 1..threads");
            check(threads.insert(decision.thread_id).second, "Public thread slot used twice");
        }
        scheduler->terminate();
    }
}

void test_rejected_candidates_after_deadline_and_state_changes() {
    {
        auto spec = system(1);
        auto scheduler = create_scheduler();
        scheduler->set_system_spec(spec);
        scheduler->arrive_at(0, {{1, 0, 11, {job(1, 5, 1000), job(2, 5, 10000)}}});
        const auto decisions = std::get<0>(scheduler->commit_until(0));
        check(decisions.size() == 1 && decisions[0].job_id == 2,
              "Rejecting an impossible deadline hid a feasible equal-work job");
        scheduler->terminate();
    }
    {
        auto spec = system(-1);
        spec.cores[0].sram_write_alpha = 0;
        spec.dram_units[0].read_beta = 0;
        auto scheduler = create_scheduler();
        scheduler->set_system_spec(spec);
        scheduler->arrive_at(0, {{1, 0, 11, {job(1, 100), job(2, 100)}}});
        const auto first = std::get<0>(scheduler->commit_until(0));
        check(first.size() == 1 && first[0].job_id == 1, "Negative-SMT setup failed");
        const auto later = std::get<0>(scheduler->commit_until(20500));
        check(later.size() == 1 && later[0].job_id == 2,
              "A rejected job was not reconsidered after the running job completed");
        scheduler->terminate();
    }
    {
        class MixedSymbiosis final : public SymbiosisSpec {
            double get_symbiosis(std::uint8_t, const std::multiset<std::uint8_t>& types) const override {
                return types == std::multiset<std::uint8_t>{0, 1} ? -1 : 1;
            }
        };
        auto spec = system();
        spec.cores[0].threads = 3;
        spec.cores[0].sram_write_alpha = 0;
        spec.dram_units[0].read_beta = 0;
        spec.symbiosis_repo = std::make_shared<MixedSymbiosis>();
        auto scheduler = create_scheduler();
        scheduler->set_system_spec(spec);
        scheduler->arrive_at(0, {{1, 0, 11, {job(1, 100)}}});
        check(std::get<0>(scheduler->commit_until(0)).size() == 1, "Mixed-SMT setup failed");
        auto rejected = job(1, 100), enabling = job(2, 100), equal_work = job(3, 100);
        rejected.type = equal_work.type = 1;
        enabling.type = 2;
        rejected.priority = 0;
        enabling.priority = 1;
        equal_work.priority = 2;
        scheduler->arrive_at(500, {{2, 500, 11, {rejected, enabling, equal_work}}});
        const auto decisions = std::get<0>(scheduler->commit_until(500));
        check(decisions.size() == 2 && decisions[0].job_id == 2 && decisions[1].job_id == 3,
              "A changed type or accepted launch incorrectly reused an earlier rejection");
        scheduler->terminate();
    }
}

void test_priority_and_inherited_deadline() {
    auto spec = system();
    spec.cores[0].threads = 1;
    auto low = job(1), high = job(2);
    low.priority = 100;
    high.priority = 0;
    auto r = replay(spec, {{1, 0, 11, {low, high}}});
    check(r.passed() && r.decisions[0].job_id == 2, "Priority direction incorrect");
    auto parent = job(80, 5, 50000), child = job(10, 5, 6000), unrelated = job(3, 5, 20000);
    parent.priority = 100;
    child.deps = {80};
    unrelated.priority = 0;
    r = replay(spec, {{1, 0, 11, {unrelated, child, parent}}});
    check(r.passed() && r.decisions[0].job_id == 80, "Downstream deadline not inherited");
}

void test_compatibility_and_memory_scheduler() {
    auto spec = system();
    spec.cores[0].allowed_types = {0};
    spec.cores[0].dram_ids = {11};
    spec.cores[0].sram_capacity_kb = 200;
    spec.cores.push_back(core(99));
    spec.cores[1].allowed_types = {1};
    spec.cores[1].dram_ids = {25};
    auto typed = job(1);
    typed.type = 1;
    const auto r = replay(spec, {{1, 0, 11, {job(1), job(2)}}, {2, 0, 25, {typed}}});
    check(r.passed(), "Compatibility replay");
    for (const auto& decision : r.decisions) {
        check(decision.core_id == (decision.job_set_id == 1 ? 7 : 99), "Wrong compatible core");
    }
}

void test_smt_selection_and_deadline_protection() {
    auto spec = system(1);
    spec.dram_units[0].read_beta = 0;
    auto good = replay(spec, {{1, 0, 11, {job(1, 100), job(2, 100)}}});
    check(good.passed() && good.decisions[1].start_time_ns == 0, "Perfect SMT not used");
    spec.symbiosis_repo = std::make_shared<Symbiosis>(-1);
    auto bad = replay(spec, {{1, 0, 11, {job(1, 100), job(2, 100)}}});
    check(bad.passed() && bad.decisions[1].start_time_ns > 0, "Destructive SMT not deferred");
    auto urgent = job(1, 100, 22000);
    auto later = job(1, 100, 100000);
    const auto protected_run = replay(spec, {{1, 0, 11, {urgent}}, {2, 1000, 11, {later}}});
    check(protected_run.passed(), "New arrival broke running deadline");
}

void test_ignored_wakeup_and_zero_budget() {
    auto spec = system();
    spec.cores[0].threads = 1;
    auto child = job(2);
    child.deps = {1};
    for (bool honor : {false, true}) {
        spec.scheduling_budget = honor ? 0 : 100;
        const auto r = replay(spec, {{1, 0, 11, {job(1), child}}}, 10000, honor);
        check(r.passed() && r.decisions[1].start_time_ns == 10000, "Ignored wakeup caused past scheduling");
    }
    spec.scheduling_budget = 1;
    const auto r = replay(spec, {{1, 0, 11, {job(1), child}}}, 10000);
    check(r.passed(), "Exhausted budget replay");
}

void test_multicore_scheduler_with_contention() {
    auto spec = system();
    spec.cores[0].threads = 1;
    spec.cores.push_back(core(99, 1));
    const auto r = replay(spec, {{1, 0, 11, {job(1), job(2)}}});
    check(r.passed() && r.decisions.size() == 2, "Multicore replay");
    check(r.decisions[0].start_time_ns == 0 && r.decisions[1].start_time_ns == 0,
          "DRAM contention incorrectly prohibited an idle core");
}

void test_empty_repeated_and_invalid_input() {
    auto spec = system();
    auto scheduler = create_scheduler();
    scheduler->set_system_spec(spec);
    scheduler->arrive_at(0, {{1, 0, 11, {}}});
    check(std::get<0>(scheduler->commit_until(0)).empty(), "Empty job set");
    scheduler->arrive_at(0, {{2, 0, 11, {job(10)}}});
    check(std::get<0>(scheduler->commit_until(0)).size() == 1, "Initial scheduling");
    check(std::get<0>(scheduler->commit_until(0)).empty(), "Duplicate commit relaunched a job");
    scheduler->terminate();
    for (bool cycle : {false, true}) {
        auto invalid = job(1);
        invalid.deps = {static_cast<std::uint16_t>(cycle ? 1 : 99)};
        scheduler = create_scheduler();
        scheduler->set_system_spec(spec);
        bool rejected = false;
        try { scheduler->arrive_at(0, {{1, 0, 11, {invalid}}}); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Invalid graph accepted");
    }
    auto incompatible = job(1);
    incompatible.type = 255;
    check(replay(spec, {{1, 0, 11, {incompatible}}}).decisions.empty(), "Incompatible job launched");
    check(!replay(spec, {{1, 0, 11, {job(1, 100, 10)}}}).passed(), "Impossible deadline reported as success");
}

void test_determinism_and_fractional_time() {
    auto spec = system();
    spec.cores[0].computing_power = 3;
    auto child = job(9, 1);
    child.deps = {8};
    const std::vector<JobSetSpec> sets{{1, 0, 11, {child, job(8, 1), job(2, 2)}}};
    const auto a = replay(spec, sets), b = replay(spec, sets);
    check(a.passed() && b.passed() && a.decisions.size() == b.decisions.size(), "Fractional run");
    for (std::size_t i = 0; i < a.decisions.size(); ++i) {
        const auto& x = a.decisions[i];
        const auto& y = b.decisions[i];
        check(std::tie(x.job_set_id, x.job_id, x.core_id, x.thread_id, x.start_time_ns) ==
              std::tie(y.job_set_id, y.job_id, y.core_id, y.thread_id, y.start_time_ns), "Nondeterministic output");
    }
}

void test_scale() {
    auto spec = system(1);
    spec.cores.clear();
    for (std::uint8_t i = 0; i < 32; ++i) {
        auto c = core(static_cast<std::uint8_t>(i + 1), 4);
        c.computing_power = 100;
        c.sram_capacity_kb = 16384;
        c.sram_read_alpha = c.sram_write_alpha = 0;
        spec.cores.push_back(c);
    }
    spec.dram_units[0].read_beta = spec.dram_units[0].write_beta = 0;
    spec.scheduling_budget = 1000;
    std::vector<JobSetSpec> sets;
    for (std::uint16_t set_id = 1; set_id <= 20; ++set_id) {
        JobSetSpec set{set_id, 0, 11, {}};
        for (std::uint16_t id = 1; id <= 500; ++id) set.jobs.push_back(job(id, 100, 1'000'000'000));
        sets.push_back(std::move(set));
    }
    const auto start = std::chrono::steady_clock::now();
    const auto r = replay(spec, sets);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    check(r.passed() && r.decisions.size() == 10000, "10000-job replay failed");
    std::cout << "[scale] jobs=10000 cores=32 threads=128 local_harness_seconds=" << elapsed
              << " commits=" << r.commits << '\n';
}

void test_varied_scale() {
    auto spec = system(0.7);
    spec.cores.clear();
    for (std::uint8_t i = 0; i < 32; ++i) {
        auto c = core(static_cast<std::uint8_t>(i + 1), 4);
        c.computing_power = 25 + 2 * i;
        c.mc_power = 25 + i;
        c.sram_capacity_kb = 16384;
        c.sram_chunk_size_kb = 64 + 16 * i;
        spec.cores.push_back(c);
    }
    spec.scheduling_budget = 1000;
    std::vector<JobSetSpec> sets;
    for (std::uint16_t set_id = 1; set_id <= 20; ++set_id) {
        JobSetSpec set{set_id, static_cast<std::uint64_t>(set_id) * 1000,
                      static_cast<std::uint8_t>(set_id % 2 == 0 ? 11 : 25), {}};
        for (std::uint16_t id = 1; id <= 500; ++id) {
            auto item = job(id, 1 + (id * 37 + set_id * 7) % 10000, 1'000'000'000);
            item.load_size_kb = static_cast<std::uint16_t>(1 + (id * 31) % 1024);
            item.write_size_kb = static_cast<std::uint16_t>((id * 13) % 1024);
            item.type = static_cast<std::uint8_t>(id % 3);
            item.priority = static_cast<std::uint8_t>(id % 101);
            if (id > 10) item.deps.push_back(static_cast<std::uint16_t>(id - 10));
            set.jobs.push_back(item);
        }
        sets.push_back(std::move(set));
    }
    const auto start = std::chrono::steady_clock::now();
    const auto r = replay(spec, sets);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    check(r.passed() && r.decisions.size() == 10000, "Varied 10000-job replay failed");
    std::cout << "[varied-scale] jobs=10000 DAGs=20 cores=32 DRAMs=2 local_harness_seconds="
              << elapsed << " commits=" << r.commits << '\n';
}

void test_negative_symbiosis_scale() {
    SystemSpec spec{};
    spec.scheduling_budget = 1000;
    spec.scheduling_period = 100;
    spec.dram_units = {{1, 0, 0}};
    spec.symbiosis_repo = std::make_shared<Symbiosis>(-1);
    for (std::uint8_t id = 1; id <= 32; ++id) {
        CoreSpec c{};
        c.id = id;
        c.threads = 4;
        c.computing_power = c.mc_power = 100;
        c.sram_capacity_kb = 16384;
        c.sram_chunk_size_kb = 64;
        c.mc_read_cost_to_sram = c.mc_write_cost_to_sram = 1;
        c.mc_read_cost_to_dram = c.mc_write_cost_to_dram = 1;
        c.allowed_types = {1};
        c.dram_ids = {1};
        spec.cores.push_back(c);
    }
    std::vector<JobSetSpec> sets;
    for (std::uint16_t set_id = 1; set_id <= 20; ++set_id) {
        JobSetSpec set{set_id, 0, 1, {}};
        for (std::uint16_t id = 1; id <= 500; ++id) {
            set.jobs.push_back({id, 1, 64, 0, 10000, 50, 1'000'000'000, {}});
        }
        sets.push_back(std::move(set));
    }
    const auto start = std::chrono::steady_clock::now();
    const auto r = replay(spec, sets);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    check(r.passed() && r.decisions.size() == 10000, "Negative-SMT 10000-job replay failed");
    std::cout << "[negative-smt-scale] jobs=10000 initially_ready=10000 cores=32 threads=128 local_harness_seconds="
              << elapsed << " commits=" << r.commits << '\n';
}

int demo() {
    auto spec = system(0.8);
    spec.cores.push_back(core(42, 2));
    spec.cores[1].computing_power = 10;
    auto a = job(1, 20, 100000), b = job(2, 10, 100000), c = job(3, 15, 100000);
    a.write_size_kb = 512;
    b.deps = {1};
    c.deps = {1, 2};
    auto urgent = job(1, 5, 20000);
    urgent.priority = 0;
    const auto r = replay(spec, {{1, 0, 11, {c, a, b, job(4, 30, 100000)}},
                                  {2, 2000, 25, {urgent, job(2, 10, 30000)}}});
    std::cout << "Local MVP demo (public-spec model; not official scoring)\n";
    std::cout << "set job core thread start_ns finish_ns\n";
    for (const auto& decision : r.decisions) {
        const Key key{decision.job_set_id, decision.job_id};
        std::cout << decision.job_set_id << ' ' << decision.job_id << ' '
                  << static_cast<unsigned>(decision.core_id) << ' '
                  << static_cast<unsigned>(decision.thread_id) << ' '
                  << decision.start_time_ns << ' ' << r.finishes.at(key).approximate_time_ns() << '\n';
    }
    std::cout << "completed=" << r.finishes.size() << '/' << r.expected
              << " commits=" << r.commits << " status=" << (r.passed() ? "PASS" : "FAIL") << '\n';
    return r.passed() ? 0 : 1;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--demo") return demo();
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"PDF memory examples", test_pdf_memory_examples},
        {"PDF symbiosis example", test_pdf_symbiosis_example},
        {"SMT extremes and duplicate types", test_symbiosis_extremes_and_multisets},
        {"Independent memory directions", test_independent_read_write},
        {"Shared DRAM across heterogeneous cores", test_shared_dram_different_cores},
        {"SRAM contention and admission", test_sram_dominant_and_admission},
        {"Restart and time boundaries", test_restart_and_time_boundaries},
        {"Integer time anchors and fractional completion", test_integer_time_anchor},
        {"Latest-start ordering and dependencies at large origins", test_latest_start_order_at_large_origins},
        {"DAG, arrivals, sparse IDs", test_dag_arrivals_and_ids},
        {"Public thread IDs 1..threads", test_public_thread_ids},
        {"Rejected candidates after deadline and state changes", test_rejected_candidates_after_deadline_and_state_changes},
        {"Priority and inherited deadlines", test_priority_and_inherited_deadline},
        {"Scheduler compatibility and SRAM", test_compatibility_and_memory_scheduler},
        {"SMT selection and running deadlines", test_smt_selection_and_deadline_protection},
        {"Ignored wakeups and budget", test_ignored_wakeup_and_zero_budget},
        {"Multicore scheduler with DRAM contention", test_multicore_scheduler_with_contention},
        {"Empty, repeated and invalid input", test_empty_repeated_and_invalid_input},
        {"Determinism and fractional events", test_determinism_and_fractional_time},
        {"10000 jobs", test_scale},
        {"10000 varied jobs with DAGs", test_varied_scale},
        {"10000 ready jobs with negative symbiosis", test_negative_symbiosis_scale},
    };
    std::size_t passed = 0;
    for (const auto& test : tests) {
        try { test.second(); ++passed; std::cout << "[PASS] " << test.first << '\n'; }
        catch (const std::exception& error) { std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n'; }
    }
    std::cout << passed << '/' << tests.size() << " tests passed\n";
    return passed == tests.size() ? 0 : 1;
}
