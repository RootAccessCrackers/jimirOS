/* HTAS - Hint-Based Topology-Aware Scheduler
 * Research Implementation for jimirOS
 * 
 * This scheduler introduces:
 * 1. Topology-awareness (simulated P-cores, E-cores, NUMA)
 * 2. Intent-based hints (PERFORMANCE, EFFICIENCY, LOW_LATENCY)
 * 3. Policy enforcement based on application hints
 */

#ifndef _KERNEL_HTAS_H
#define _KERNEL_HTAS_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declaration to avoid circular dependency */
struct process;

/* ============================================================================
 * SIMULATED HARDWARE TOPOLOGY
 * ============================================================================ */

#define NUM_CPUS 4
#define NUM_NUMA_NODES 2

typedef enum {
    CPU_TYPE_PCORE,  // Performance core (fast)
    CPU_TYPE_ECORE,  // Efficiency core (slow, power-saving)
} cpu_type_t;

typedef struct {
    uint8_t cpu_id;
    cpu_type_t type;
    uint8_t numa_node;
    bool online;
} cpu_info_t;

/* Hard-coded topology map:
 * CPU 0: P-Core, NUMA Node 0
 * CPU 1: P-Core, NUMA Node 0
 * CPU 2: E-Core, NUMA Node 1
 * CPU 3: E-Core, NUMA Node 1
 */
extern cpu_info_t g_cpu_topology[NUM_CPUS];

/* NUMA memory regions (simulated)
 * Node 0: 0x00000000 - 0x07FFFFFF (128MB)
 * Node 1: 0x08000000 - 0x0FFFFFFF (128MB)
 */
typedef struct {
    uint32_t base;
    uint32_t size;
} numa_region_t;

extern numa_region_t g_numa_regions[NUM_NUMA_NODES];

/* ============================================================================
 * TASK INTENT PROFILES
 * ============================================================================ */

typedef enum {
    PROFILE_PERFORMANCE,   // CPU-bound, long-running (e.g., video render)
    PROFILE_EFFICIENCY,    // Background task (e.g., file indexing)
    PROFILE_LOW_LATENCY,   // Interactive task (e.g., UI, game loop)
    PROFILE_DEFAULT,       // No hint provided
} task_intent_t;

typedef struct {
    task_intent_t intent;
    void* primary_data_region;  // For NUMA-awareness
    uint32_t data_size;          // Size of data region
} task_profile_t;

/* ============================================================================
 * SCHEDULER TYPES
 * ============================================================================ */

typedef enum {
    SCHED_BASELINE,   // Round-robin, topology-unaware
    SCHED_HTAS,       // Hint-based topology-aware
    SCHED_DYNAMIC,    // Dynamic scheduler
} scheduler_type_t;

/* ============================================================================
 * PROCESS EXTENSIONS FOR HTAS
 * ============================================================================ */

typedef struct htas_task_info {
    task_profile_t profile;
    uint32_t cpu_affinity_mask;     // Bitmask of allowed CPUs
    int priority_boost;              // For LOW_LATENCY tasks
    uint8_t preferred_numa_node;    // Calculated from data_region
    
    // Aging mechanism
    uint32_t wait_time;              // Ticks waited without running
    int priority_boost_aging;        // Temporary boost from aging
    
    // Statistics
    uint64_t total_runtime_us;
    uint64_t total_switches;
    uint64_t numa_penalties;         // Cross-NUMA access penalties
} htas_task_info_t;

/* ============================================================================
 * SIMULATION PARAMETERS
 * ============================================================================ */

#define ECORE_SLOWDOWN_FACTOR 2      // E-cores run at 50% speed
#define NUMA_PENALTY_CYCLES 100      // Cross-NUMA access penalty
#define LOW_LATENCY_PRIORITY_BOOST 10

#define AGING_THRESHOLD 100          // Ticks before aging boost
#define AGING_PRIORITY_BOOST 5       // Boost amount for aged tasks

/* ============================================================================
 * API FUNCTIONS
 * ============================================================================ */

/* Initialize HTAS scheduler */
void htas_init(void);

/* Set the active scheduler type */
void htas_set_scheduler(scheduler_type_t type);

/* Get current scheduler type */
scheduler_type_t htas_get_scheduler(void);

/* System call: Set task profile hint */
int sys_sched_set_profile(uint32_t pid, const task_profile_t* profile);

/* Scheduler tick integration helpers */
struct process* htas_pick_next_process(struct process* current);
void htas_record_switch(struct process* current, struct process* next);

/* NUMA-aware memory access simulation */
void htas_simulate_memory_access(struct process* proc, void* addr, uint32_t size);

/* Get CPU type */
cpu_type_t htas_get_cpu_type(uint8_t cpu_id);

/* Get NUMA node for address */
uint8_t htas_get_numa_node_for_address(void* addr);

/* Get NUMA node for CPU */
uint8_t htas_get_numa_node_for_cpu(uint8_t cpu_id);

/* Calculate CPU affinity mask based on profile */
uint32_t htas_calculate_affinity(const task_profile_t* profile);

/* Check if process can run on CPU */
bool htas_can_run_on_cpu(struct process* proc, uint8_t cpu_id);

/* Select next process to run (HTAS algorithm) */
struct process* htas_select_next(uint8_t cpu_id);

/* Baseline round-robin selection */
struct process* baseline_select_next(void);

/* ============================================================================
 * STATISTICS & BENCHMARKING
 * ============================================================================ */

typedef struct {
    uint64_t total_ticks;
    uint64_t context_switches;
    uint64_t numa_penalties;
    uint64_t ecore_time_us;
    uint64_t pcore_time_us;
    
    // Per-intent statistics
    struct {
        uint64_t runtime_us;
        uint64_t switches;
        uint64_t avg_latency_us;  // For LOW_LATENCY tasks
        uint64_t max_jitter_us;   // For LOW_LATENCY tasks
    } intent_stats[4];  // PERFORMANCE, EFFICIENCY, LOW_LATENCY, DEFAULT
    
    // Power simulation (arbitrary units)
    uint64_t total_power_consumption;
} scheduler_stats_t;

extern scheduler_stats_t g_baseline_stats;
extern scheduler_stats_t g_htas_stats;

/* Get current statistics */
scheduler_stats_t* htas_get_stats(void);

/* Reset statistics */
void htas_reset_stats(void);

/* Print statistics */
void htas_print_stats(scheduler_stats_t* stats, const char* name);

/* Compare two stat sets */
void htas_compare_stats(scheduler_stats_t* stats_a, const char* name_a,
                        scheduler_stats_t* stats_b, const char* name_b);

/* ============================================================================
 * SHELL COMMAND WRAPPERS
 * ============================================================================ */

/* Run full benchmark comparison */
void htas_run_full_benchmark(void);

/* Run baseline-only benchmark */
void htas_run_baseline_benchmark(void);

/* Run HTAS-only benchmark */
void htas_run_htas_benchmark(void);

/* Print hardware topology */
void htas_print_topology(void);

#endif /* _KERNEL_HTAS_H */
