#include "spec_scheduler.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <sstream>
#include <set>
#include <limits>

using namespace arena;

// Simplified runner example: hardcoded dadaset
// run contestant IScheduler with explicit sequential arrive_at / commit_until calls.

// Serializes a single scheduling decision to output stream
// Format: job_set_id job_id core_id thread_id start_time_ns
std::ostream& operator<<(std::ostream& os, const SchedulerDecision& decision) {
    os << decision.job_set_id << " "
       << decision.job_id << " "
       << static_cast<int>(decision.core_id) << " "
       << static_cast<int>(decision.thread_id) << " "
       << decision.start_time_ns;
    return os;
}

// SymbiosisSpec implementation with hardcoded data
class SymbiosisSpecImpl : public SymbiosisSpec {
public:
    double get_symbiosis(std::uint8_t core_id, const std::multiset<std::uint8_t>& types) const override {
        auto core_it = symbiosis_data_.find(core_id);
        if (core_it == symbiosis_data_.end()) return 0.0;
        auto type_it = core_it->second.find(types);
        if (type_it == core_it->second.end()) return 0.0;
        return type_it->second;
    }

    static const std::map<std::uint8_t, std::map<std::multiset<std::uint8_t>, double>>& get_all_data() {
        static const std::map<std::uint8_t, std::map<std::multiset<std::uint8_t>, double>> data = []{
            std::map<std::uint8_t, std::map<std::multiset<std::uint8_t>, double>> result;

            auto add = [&](std::uint8_t core, std::initializer_list<std::uint8_t> types_list, double value) {
                std::multiset<std::uint8_t> types(types_list);
                result[core][types] = value;
            };

            add(1, {0}, 1.0);
            add(1, {1}, 1.0);
            add(1, {2}, 1.0);
            add(1, {3}, 1.0);

            add(2, {0}, 1.0);
            add(2, {1}, 1.0);
            add(2, {2}, 1.0);
            add(2, {3}, 1.0);

            add(3, {0}, 1.0);
            add(3, {0, 0}, 0.0310408);
            add(3, {1}, 1.0);
            add(3, {1, 2}, 1.0);
            add(3, {2, 2}, -0.175605);
            add(3, {2}, 0.563557);
            add(3, {3}, 1.0);

            add(4, {0}, 1.0);
            add(4, {1}, 1.0);
            add(4, {2}, 1.0);
            add(4, {2, 2}, 0.488166);
            add(4, {3}, 1.0);

            return result;
        }();
        return data;
    }
private:
    const std::map<std::uint8_t, std::map<std::multiset<std::uint8_t>, double>>& symbiosis_data_ = get_all_data();
};

int main() {
    try {
        const auto start_time = std::chrono::high_resolution_clock::now();

        // ---------- system spec ----------
        SystemSpec system;

        // Core 1 spec
        CoreSpec c1; c1.id = 1; c1.computing_power = 35.7389; c1.threads = 1;
        c1.sram_capacity_kb = 3978; c1.sram_chunk_size_kb = 939;
        c1.mc_power = 57.4149; c1.mc_read_cost_to_sram = 63.5232;
        c1.mc_write_cost_to_sram = 8.94541; c1.mc_read_cost_to_dram = 55.6179;
        c1.mc_write_cost_to_dram = 78.9652;
        c1.sram_read_alpha = 0.470752; c1.sram_write_alpha = 0.074425;
        c1.allowed_types = {0, 1, 2, 3}; c1.dram_ids = {1};

        // Core 2 spec
        CoreSpec c2; c2.id = 2; c2.computing_power = 42.4482; c2.threads = 1;
        c2.sram_capacity_kb = 4280; c2.sram_chunk_size_kb = 304;
        c2.mc_power = 47.9848; c2.mc_read_cost_to_sram = 26.994;
        c2.mc_write_cost_to_sram = 28.6043; c2.mc_read_cost_to_dram = 74.8991;
        c2.mc_write_cost_to_dram = 45.8125;
        c2.sram_read_alpha = 0.291865; c2.sram_write_alpha = 0.803236;
        c2.allowed_types = {0, 1, 2, 3}; c2.dram_ids = {1};

        // Core 3 spec
        CoreSpec c3; c3.id = 3; c3.computing_power = 32.8542; c3.threads = 2;
        c3.sram_capacity_kb = 8227; c3.sram_chunk_size_kb = 172;
        c3.mc_power = 69.7813; c3.mc_read_cost_to_sram = 64.7797;
        c3.mc_write_cost_to_sram = 79.0206; c3.mc_read_cost_to_dram = 39.2525;
        c3.mc_write_cost_to_dram = 52.9938;
        c3.sram_read_alpha = 0.119353; c3.sram_write_alpha = 0.069119;
        c3.allowed_types = {0, 1, 2, 3}; c3.dram_ids = {1};

        // Core 4 spec
        CoreSpec c4; c4.id = 4; c4.computing_power = 19.8454; c4.threads = 2;
        c4.sram_capacity_kb = 7064; c4.sram_chunk_size_kb = 637;
        c4.mc_power = 4.80538; c4.mc_read_cost_to_sram = 28.3266;
        c4.mc_write_cost_to_sram = 88.5305; c4.mc_read_cost_to_dram = 26.1776;
        c4.mc_write_cost_to_dram = 52.0486;
        c4.sram_read_alpha = 0.88842; c4.sram_write_alpha = 0.370605;
        c4.allowed_types = {0, 1, 2, 3}; c4.dram_ids = {1};

        system.cores = {c1, c2, c3, c4};

        // DRAM spec
        DRAMSpec dram1; dram1.id = 1; dram1.read_beta = 0.136407;
        dram1.write_beta = 0.451215;
        system.dram_units = {dram1};

        // Scheduling params
        system.scheduling_period = 11178;
        system.gamma_border = 18232;
        system.scheduling_budget = 100;

        system.symbiosis_repo = std::make_shared<SymbiosisSpecImpl>();

        // ---------- job set 1 (arrival 0, 12 jobs) ----------
        JobSetSpec js1;
        js1.id = 1; js1.arrival_ns = 0; js1.dram_id = 1;

        { JobSpec j; j.id=1; j.type=2; j.load_size_kb=637; j.write_size_kb=1911;
          j.compute_units=44; j.priority=67; j.relative_deadline_ns=34778;
          js1.jobs.push_back(j); }
        { JobSpec j; j.id=2; j.type=1; j.load_size_kb=2048; j.write_size_kb=1720;
          j.compute_units=57; j.priority=0; j.relative_deadline_ns=33105;
          j.deps = {9}; js1.jobs.push_back(j); }
        { JobSpec j; j.id=3; j.type=2; j.load_size_kb=1878; j.write_size_kb=1878;
          j.compute_units=50; j.priority=100; j.relative_deadline_ns=12538;
          js1.jobs.push_back(j); }
        { JobSpec j; j.id=4; j.type=3; j.load_size_kb=1216; j.write_size_kb=912;
          j.compute_units=93; j.priority=90; j.relative_deadline_ns=15870;
          js1.jobs.push_back(j); }
        { JobSpec j; j.id=5; j.type=2; j.load_size_kb=1892; j.write_size_kb=1892;
          j.compute_units=88; j.priority=77; j.relative_deadline_ns=18389;
          js1.jobs.push_back(j); }
        { JobSpec j; j.id=6; j.type=0; j.load_size_kb=939; j.write_size_kb=1878;
          j.compute_units=45; j.priority=96; j.relative_deadline_ns=3212;
          js1.jobs.push_back(j); }
        { JobSpec j; j.id=7; j.type=0; j.load_size_kb=637; j.write_size_kb=1911;
          j.compute_units=22; j.priority=72; j.relative_deadline_ns=9668;
          js1.jobs.push_back(j); }
        { JobSpec j; j.id=8; j.type=2; j.load_size_kb=1878; j.write_size_kb=1878;
          j.compute_units=119; j.priority=100; j.relative_deadline_ns=18990;
          js1.jobs.push_back(j); }
        { JobSpec j; j.id=9; j.type=0; j.load_size_kb=1878; j.write_size_kb=1878;
          j.compute_units=81; j.priority=50; j.relative_deadline_ns=8636;
          js1.jobs.push_back(j); }
        { JobSpec j; j.id=10; j.type=2; j.load_size_kb=637; j.write_size_kb=1911;
          j.compute_units=29; j.priority=22; j.relative_deadline_ns=84667;
          js1.jobs.push_back(j); }
        { JobSpec j; j.id=11; j.type=3; j.load_size_kb=1824; j.write_size_kb=1824;
          j.compute_units=58; j.priority=0; j.relative_deadline_ns=40660;
          js1.jobs.push_back(j); }
        { JobSpec j; j.id=12; j.type=1; j.load_size_kb=860; j.write_size_kb=1204;
          j.compute_units=92; j.priority=100; j.relative_deadline_ns=13352;
          js1.jobs.push_back(j); }

        // ---------- run scheduler ----------
        const std::unique_ptr<IScheduler> scheduler = create_scheduler();
        scheduler->set_system_spec(system);

        const uint64_t commit_times[] = {
            0, 11178, 22356, 33534, 44712, 55890,
            67068, 78246, 89424, 100602, 111780, 122958,
            134136, 145314, 156492, 167670, 178848, 190026,
            201204, 212382, 223560, 234738, 245916, 257094,
            268272, 279450, 290628, 301806, 312984, 324162,
            335340, 346518, 357696, 368874, 380052, 391230,
            402408, 413586, 424764, 435942, 447120, 458298,
            469476, 480654, 491832, 503010, 514188, 525366,
            536544, 547722, 558900, 570078, 581256, 592434,
        };

        // arrive_at at t=0 with the only job set
        scheduler->arrive_at(0, {js1});

        // get scheduling decisions over period
        for (uint64_t t : commit_times) {
            auto [decisions, requested] = scheduler->commit_until(t);
            for (const auto& d : decisions) {
                std::cout << d << std::endl;
            }
            (void)requested;
        }

        scheduler->terminate();

        auto end_time = std::chrono::high_resolution_clock::now();
        double scheduling_runtime = std::chrono::duration<double>(end_time - start_time).count();

        // show scheduler results
        std::cerr << "[runner_example] cores=" << system.cores.size()
                  << " jobs=12"
                  << " scheduling_runtime=" << std::fixed << std::setprecision(6)
                  << scheduling_runtime << "s" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Error: Unknown exception caught" << std::endl;
        return 1;
    }
}
