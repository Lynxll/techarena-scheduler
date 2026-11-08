#ifndef TECHARENA_SPEC_SYSTEM_H
#define TECHARENA_SPEC_SYSTEM_H

#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <set>

namespace arena {

// ==================================================
// DRAM input parameters
// ==================================================
struct DRAMSpec {
    std::uint8_t  id;         // Unique DRAM unit id
    double        read_beta;  // beta^r: global read serialization
    double        write_beta; // beta^w: global write serialization
};

// ==================================================
// Symbiosis repository (interface sketch).
// Provides s_{c; i_1, ..., i_k} for a given core and
// a multiset of job types.
// ==================================================
struct SymbiosisSpec {
    virtual ~SymbiosisSpec() = default;
    virtual double get_symbiosis(
        std::uint8_t                        core_id,
        const std::multiset<std::uint8_t>&  types
    ) const = 0;
};

// ==================================================
// Input data for a job and job set
// The full implementation is entirely up to the contestant.
// ==================================================
struct JobSpec {
    std::uint16_t id;                     // Job id within the set (> 0)
    std::uint8_t  type;                   // Job type
    std::uint16_t load_size_kb;           // l_i: data to load from DRAM (KB)
    std::uint16_t write_size_kb;          // w_i: data to write to DRAM (KB)
    std::uint32_t compute_units;          // e_i
    std::uint8_t  priority;               // p_i in [0, 100]
    std::uint64_t relative_deadline_ns;   // d_i: relative to arrival, ns
    std::vector<std::uint16_t> deps;      // predecessor job ids (within the same set)
};

// ==================================================
// Job set specification
// ==================================================
struct JobSetSpec {
    std::uint16_t  id;            // Job set id (> 0)
    std::uint64_t  arrival_ns;    // a_k: arrival time, ns
    std::uint8_t   dram_id;       // DRAM unit where data resides
    std::vector<JobSpec> jobs;
};

// ==================================================
// Core input parameters
// ==================================================
struct CoreSpec {
    std::uint8_t    id;                          // Core index
    double          computing_power;             // P_i: units/mus
    std::uint8_t    threads;                     // M_i
    std::uint32_t   sram_capacity_kb;            // S_i
    std::uint32_t   sram_chunk_size_kb;          // K_i
    double          mc_power;                    // H_i: units/mus
    double          mc_read_cost_to_sram;
    double          mc_write_cost_to_sram;
    double          mc_read_cost_to_dram;
    double          mc_write_cost_to_dram;
    double          sram_read_alpha;             // alpha^r_i
    double          sram_write_alpha;            // alpha^w_i
    std::vector<std::uint32_t> allowed_types;    // allowed types of jobs
    std::vector<std::uint8_t> dram_ids;          // connected DRAM units
};

// ==================================================
// Full System and DRAM Configuration
// ==================================================
struct SystemSpec {
    std::vector<CoreSpec>                   cores;
    std::vector<DRAMSpec>                   dram_units;
    std::uint64_t                           scheduling_period;  // t_sched in microseconds
    std::uint64_t                           gamma_border;       // for variability in scheduling_period
    std::uint64_t                           scheduling_budget;  // for user-defined calls limit
    std::shared_ptr<const SymbiosisSpec>    symbiosis_repo;     // symbiosis coefficients per core
};

} // namespace arena

#endif // TECHARENA_SPEC_SYSTEM_H
