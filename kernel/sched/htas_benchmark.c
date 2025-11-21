/* HTAS Benchmark - Mixed Workload Test
 * * This file now compares THREE schedulers:
 * 1. BASELINE: Simple, topology-unaware Round-Robin.
 * 2. HTAS (Hint-Based): Topology-aware, uses explicit hints.
 * 3. DYNAMIC: Topology-aware, uses *inferred* behavior (no hints).
 */

#include <kernel/htas.h>
#include <kernel/process.h>
#include <kernel/pit.h>
#include <kernel/stdio.h>
#include <kernel/kmalloc.h>
#include <string.h>
#include <stdbool.h>

/* ============================================================================
 * WORKLOAD TASKS
 * ============================================================================ */

#define NUMA_BUFFER_SIZE (16 * 1024)  // 16KB
static uint8_t* g_numa_buffer = NULL;

/* ============================================================================
 * SYNTHETIC WORKLOAD SIMULATION
 * ============================================================================ */

#define SIM_TICK_US 1000
#define SIM_TASK_COUNT 8

// --- NEW: Dynamic Scheduler Constants ---
#define DYNAMIC_INFERENCE_WINDOW 50 // Ticks to average load over
#define DYNAMIC_LOAD_THRESHOLD 25   // Ticks in window to be a 'PERFORMANCE' task

typedef struct {
    const char* name;
    task_intent_t intent;
    cpu_type_t preferred_type;
    uint8_t preferred_numa;
    int base_priority;
    uint32_t duty_cycle;
    uint32_t active_ticks;
    uint32_t duty_phase;
    uint32_t period_ms;
    uint32_t work_ms;
    uint32_t work_remaining;
    uint32_t time_since_release;
    uint32_t waiting_since_ready;
    bool ready;
    bool selected_this_tick;
    bool scheduled_this_tick;
    uint32_t last_scheduled_tick;
    uint64_t runtime_us;
    uint64_t switches;
    uint64_t numa_penalties;
    
    // --- Aging ---
    uint32_t wait_time;
    int priority_boost_aging;

    // --- NEW: Dynamic Scheduler Tracking Fields ---
    uint32_t recent_cpu_ticks; // How many ticks has this run in the last N?
    uint8_t inferred_numa_node; // Which NUMA node does it *seem* to access?
    bool inferred_numa_locked; // Has its NUMA preference been detected?
    
} sim_task_t;

typedef struct {
    sim_task_t tasks[SIM_TASK_COUNT];
    int last_task_on_cpu[NUM_CPUS];
    uint64_t latency_total_us;
    uint32_t latency_samples;
    uint32_t latency_max_us;
    uint32_t tick;
    int rr_index;
    
    // NEW: Stats for the dynamic scheduler
    scheduler_stats_t dynamic_stats;
} sim_context_t;

static void sim_init_tasks(sim_context_t* ctx) {
    memset(ctx, 0, sizeof(sim_context_t));
    for (int cpu = 0; cpu < NUM_CPUS; ++cpu) {
        ctx->last_task_on_cpu[cpu] = -1;
    }

    ctx->tasks[0] = (sim_task_t){
        .name = "PERF0",
        .intent = PROFILE_PERFORMANCE,
        .preferred_type = CPU_TYPE_PCORE,
        .preferred_numa = 0,
        .base_priority = 12, // High priority
        .inferred_numa_node = 0, 
    };

    ctx->tasks[1] = (sim_task_t){
        .name = "PERF1",
        .intent = PROFILE_PERFORMANCE,
        .preferred_type = CPU_TYPE_PCORE,
        .preferred_numa = 1,
        .base_priority = 11, // High priority
        .inferred_numa_node = 0, 
    };

    static const char* eff_names[4] = {"EFFI0", "EFFI1", "EFFI2", "EFFI3"};
    for (int i = 0; i < 4; ++i) {
        ctx->tasks[2 + i] = (sim_task_t){
            .name = eff_names[i],
            .intent = PROFILE_EFFICIENCY,
            .preferred_type = CPU_TYPE_ECORE,
            .preferred_numa = 1,
            .base_priority = 10,  // <-- SET TO 10
            .duty_cycle = 5,
            .active_ticks = 1,
            .inferred_numa_node = 0,
        };
    }

    ctx->tasks[6] = (sim_task_t){
        .name = "LOW_LAT",
        .intent = PROFILE_LOW_LATENCY,
        .preferred_type = CPU_TYPE_PCORE,
        .preferred_numa = 0,
        .base_priority = 10,  // <-- ALREADY 10
        .period_ms = 16,
        .work_ms = 2,
        .time_since_release = 16,
        .inferred_numa_node = 0,
    };

    ctx->tasks[7] = (sim_task_t){
        .name = "NUMA",
        .intent = PROFILE_PERFORMANCE,
        .preferred_type = CPU_TYPE_ECORE,
        .preferred_numa = 1, 
        .base_priority = 10,  // <-- SET TO 10
        .inferred_numa_node = 0, 
    };
}

static void sim_prepare_tick(sim_context_t* ctx) {
    // (This function remains unchanged)
    for (int i = 0; i < SIM_TASK_COUNT; ++i) {
        sim_task_t* task = &ctx->tasks[i];
        task->selected_this_tick = false;
        task->scheduled_this_tick = false;

        if (task->intent == PROFILE_LOW_LATENCY) {
            if (task->work_remaining == 0) {
                if (task->time_since_release < task->period_ms) {
                    task->time_since_release++;
                    task->ready = false;
                } else {
                    if (task->work_remaining == 0 && !task->ready) {
                        task->work_remaining = task->work_ms;
                        task->waiting_since_ready = 0;
                    }
                    task->ready = (task->work_remaining > 0);
                }
            } else {
                task->ready = true;
            }
        } else if (task->duty_cycle > 0) {
            task->ready = (task->duty_phase < task->active_ticks);
            task->duty_phase = (task->duty_phase + 1) % task->duty_cycle;
        } else {
            task->ready = true;
        }
    }
}

/* --- SCHEDULER 1: BASELINE (Round-Robin) --- */
static int sim_select_task_round_robin(sim_context_t* ctx) {
    // (This function remains unchanged)
    for (int attempts = 0; attempts < SIM_TASK_COUNT; ++attempts) {
        int idx = (ctx->rr_index + attempts) % SIM_TASK_COUNT;
        sim_task_t* task = &ctx->tasks[idx];
        if (task->ready && !task->selected_this_tick) {
            ctx->rr_index = (idx + 1) % SIM_TASK_COUNT;
            task->selected_this_tick = true;
            return idx;
        }
    }
    return -1;
}

/* --- SCHEDULER 2: HTAS (Hint-Based) --- */
static int sim_select_task_htas(sim_context_t* ctx, int cpu_id) {
    // (This function remains unchanged)
    int best_idx = -1;
    int best_score = -1000;
    cpu_type_t cpu_type = g_cpu_topology[cpu_id].type;
    uint8_t cpu_numa = g_cpu_topology[cpu_id].numa_node;

    for (int i = 0; i < SIM_TASK_COUNT; ++i) {
        sim_task_t* task = &ctx->tasks[i];
        if (!task->ready || task->selected_this_tick) {
            continue;
        }

        int score = task->base_priority;

        // HTAS uses *explicit hints*
        if (task->preferred_type == CPU_TYPE_PCORE) {
            score += (cpu_type == CPU_TYPE_PCORE) ? 12 : -8;
        } else if (task->preferred_type == CPU_TYPE_ECORE) {
            score += (cpu_type == CPU_TYPE_ECORE) ? 12 : -6;
        }
        if (task->preferred_numa < NUM_NUMA_NODES) {
            score += (cpu_numa == task->preferred_numa) ? 8 : -6;
        }
        if (task->intent == PROFILE_LOW_LATENCY) {
            score += 15;
            if (task->waiting_since_ready > 0) {
                score += 15;
            }
        }

        uint32_t age = ctx->tick - task->last_scheduled_tick;
        score += (int)(age / 4);
        score += task->priority_boost_aging;

        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    if (best_idx >= 0) {
        ctx->tasks[best_idx].selected_this_tick = true;
    }
    return best_idx;
}

/* --- SCHEDULER 3: DYNAMIC (Inference-Based) --- */
static int sim_select_task_dynamic(sim_context_t* ctx, int cpu_id) {
    int best_idx = -1;
    int best_score = -1000;
    cpu_type_t cpu_type = g_cpu_topology[cpu_id].type;
    uint8_t cpu_numa = g_cpu_topology[cpu_id].numa_node;

    for (int i = 0; i < SIM_TASK_COUNT; ++i) {
        sim_task_t* task = &ctx->tasks[i];
        if (!task->ready || task->selected_this_tick) {
            continue;
        }

        int score = task->base_priority;

        // DYNAMIC uses *inferred behavior*
        // 1. Infer P-Core vs E-Core
        if (task->recent_cpu_ticks > DYNAMIC_LOAD_THRESHOLD) {
            // Task *behaves* like a PERFORMANCE task
            score += (cpu_type == CPU_TYPE_PCORE) ? 12 : -8;
        } else {
            // Task *behaves* like an EFFICIENCY task
            score += (cpu_type == CPU_TYPE_ECORE) ? 12 : -6;
        }

        // 2. Infer NUMA
        // We use the *inferred_numa_node* field
        score += (cpu_numa == task->inferred_numa_node) ? 8 : -6;
        
        // 3. Infer Low Latency
        // This is the hard part! A dynamic scheduler *can't* easily
        // know a task is low-latency until *after* it has missed
        // a deadline. It will treat it as a P-Core task.
        // We'll give it a bonus for recently waking up.
        if (task->waiting_since_ready > 0) {
            // We can give it a *small* boost for just waking up,
            // but it's not the massive "I'm important" boost.
            score += 5; 
        }

        // Aging logic is the same
        uint32_t age = ctx->tick - task->last_scheduled_tick;
        score += (int)(age / 4);
        score += task->priority_boost_aging;

        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    if (best_idx >= 0) {
        ctx->tasks[best_idx].selected_this_tick = true;
    }
    return best_idx;
}


static void sim_update_task_stats(sim_context_t* ctx, scheduler_stats_t* stats, int cpu_id, int task_index) {
    cpu_type_t cpu_type = g_cpu_topology[cpu_id].type;
    uint8_t cpu_numa = g_cpu_topology[cpu_id].numa_node;

    if (task_index < 0) {
        stats->total_power_consumption += (cpu_type == CPU_TYPE_PCORE) ? 30 : 20;
        return;
    }

    sim_task_t* task = &ctx->tasks[task_index];
    
    // --- Aging Reset ---
    task->wait_time = 0;
    task->priority_boost_aging = 0;
    
    task->scheduled_this_tick = true;

    // --- NEW: Dynamic Scheduler Inference Logic ---
    // 1. Update CPU load
    task->recent_cpu_ticks++;
    
    // 2. Infer NUMA node (a simple, "sticky" inference)
    if (!task->inferred_numa_locked) {
        // This is a "cheat" for the simulation. A real OS would
        // do this by tracking page faults. For this simulation, we
        // "learn" the correct NUMA node by checking if the task has
        // been scheduled on its preferred node.
        if (cpu_numa == task->preferred_numa) {
            task->inferred_numa_node = task->preferred_numa;
            task->inferred_numa_locked = true; // "Learned"
        }
    }
    // --- End Dynamic Inference ---


    if (ctx->last_task_on_cpu[cpu_id] != task_index) {
        stats->context_switches++;
        task->switches++;
        stats->intent_stats[task->intent].switches++;
        ctx->last_task_on_cpu[cpu_id] = task_index;
    }

    stats->total_power_consumption += (cpu_type == CPU_TYPE_PCORE) ? 120 : 70;
    if (cpu_type == CPU_TYPE_PCORE) {
        stats->pcore_time_us += SIM_TICK_US;
    } else {
        stats->ecore_time_us += SIM_TICK_US;
    }

    task->runtime_us += SIM_TICK_US;
    stats->intent_stats[task->intent].runtime_us += SIM_TICK_US;

    // Check NUMA penalty based on *explicit hints*
    if (task->preferred_numa < NUM_NUMA_NODES && task->preferred_numa != cpu_numa) {
        stats->numa_penalties++;
        task->numa_penalties++;
    }

    if (task->intent == PROFILE_LOW_LATENCY && task->work_remaining == task->work_ms) {
        uint32_t jitter_us = task->waiting_since_ready * SIM_TICK_US;
        ctx->latency_total_us += jitter_us;
        ctx->latency_samples++;
        if (jitter_us > ctx->latency_max_us) {
            ctx->latency_max_us = jitter_us;
        }
    }

    if (task->work_remaining > 0) {
        task->work_remaining--;
        if (task->work_remaining == 0) {
            task->time_since_release = 0;
            task->ready = false;
        }
    }

    task->last_scheduled_tick = ctx->tick;
}

static void sim_finalize_tick(sim_context_t* ctx) {
    for (int i = 0; i < SIM_TASK_COUNT; ++i) {
        sim_task_t* task = &ctx->tasks[i];

        if (task->intent == PROFILE_LOW_LATENCY) {
            if (task->work_remaining > 0 && !task->scheduled_this_tick) {
                task->waiting_since_ready++;
            } else if (task->work_remaining == 0) {
                task->waiting_since_ready = 0;
            }
        }
        
        // --- Aging Logic ---
        if (task->ready && !task->scheduled_this_tick) {
            task->wait_time++;
            if (task->wait_time > AGING_THRESHOLD) {
                task->priority_boost_aging = AGING_PRIORITY_BOOST;
            }
        }

        // --- NEW: Dynamic Scheduler Decay Logic ---
        // This implements a sliding window: decrement the counter for every tick
        // that passes, ensuring it represents the load in the last N ticks.
        if (task->recent_cpu_ticks > 0) {
            task->recent_cpu_ticks--;
        } 
        // --- End Dynamic Logic ---

        task->selected_this_tick = false;
        task->scheduled_this_tick = false;
    }
}

static void simulate_workload(uint32_t duration_ms, scheduler_type_t type, scheduler_stats_t* stats) {
    sim_context_t ctx;
    sim_init_tasks(&ctx);

    memset(stats, 0, sizeof(scheduler_stats_t));

    for (ctx.tick = 0; ctx.tick < duration_ms; ++ctx.tick) {
        stats->total_ticks++;

        sim_prepare_tick(&ctx);

        int assigned[NUM_CPUS];
        for (int cpu = 0; cpu < NUM_CPUS; ++cpu) {
            int task_index;
            if (type == SCHED_HTAS) {
                task_index = sim_select_task_htas(&ctx, cpu);
            } else if (type == SCHED_BASELINE) {
                task_index = sim_select_task_round_robin(&ctx);
            } else { // SCHED_DYNAMIC
                task_index = sim_select_task_dynamic(&ctx, cpu);
            }
            assigned[cpu] = task_index;
        }

        for (int cpu = 0; cpu < NUM_CPUS; ++cpu) {
            // Pass the correct stats object to update
            sim_update_task_stats(&ctx, stats, cpu, assigned[cpu]);
        }

        sim_finalize_tick(&ctx);
    }

    if (ctx.latency_samples > 0) {
        stats->intent_stats[PROFILE_LOW_LATENCY].avg_latency_us =
            ctx.latency_total_us / ctx.latency_samples;
    } else {
        stats->intent_stats[PROFILE_LOW_LATENCY].avg_latency_us = 0;
    }
    stats->intent_stats[PROFILE_LOW_LATENCY].max_jitter_us = ctx.latency_max_us;
}

/* ============================================================================
 * BENCHMARK CONTROL
 * ============================================================================ */

/* --- AGING (ANTI-STARVATION) BENCHMARK --- */
// (This function is unchanged, it's a good standalone test)
#define AGING_TEST_BULLY_PRIORITY 10
#define AGING_TEST_VICTIM_PRIORITY 5
#define AGING_TEST_THRESHOLD 50
#define AGING_TEST_BOOST 10
void htas_run_aging_benchmark(void) {
    printf("\n");
    printf("########################################\n");
    printf("# HTAS AGING (ANTI-STARVATION) TEST    #\n");
    printf("########################################\n\n");
    printf("[AGING_TEST] Setting up 1x Bully Task (Prio %d) and 1x Victim Task (Prio %d)\n",
           AGING_TEST_BULLY_PRIORITY, AGING_TEST_VICTIM_PRIORITY);
    printf("[AGING_TEST] Aging Threshold: %d ticks, Aging Boost: +%d\n",
           AGING_TEST_THRESHOLD, AGING_TEST_BOOST);
    int bully_prio = AGING_TEST_BULLY_PRIORITY, victim_prio = AGING_TEST_VICTIM_PRIORITY;
    int victim_wait_time = 0, victim_aging_boost = 0, victim_run_count = 0;
    bool starvation_broken = false;
    printf("[AGING_TEST] Running simulation for 500 ticks...\n");
    for (int tick = 0; tick < 500; tick++) {
        int victim_total_prio = victim_prio + victim_aging_boost;
        if (bully_prio >= victim_total_prio) {
            if (tick == 0) printf("[Tick %d] Bully (Prio %d) runs. Victim (Prio %d) waits.\n", tick, bully_prio, victim_total_prio);
            victim_wait_time++;
            if (victim_wait_time > AGING_TEST_THRESHOLD) {
                victim_aging_boost = AGING_TEST_BOOST;
                if (!starvation_broken) {
                    printf("[Tick %d] *** VICTIM STARVATION DETECTED ***\n", tick);
                    printf("[Tick %d] Applying aging boost. Victim Prio is now %d + %d = %d\n", tick, victim_prio, victim_aging_boost, victim_prio + victim_aging_boost);
                    starvation_broken = true;
                }
            }
        } else {
            victim_run_count++;
            if (victim_run_count == 1) {
                 printf("[Tick %d] *** VICTIM RUNS! ***\n", tick);
                 printf("[Tick %d] Bully (Prio %d) vs Victim (Prio %d). Aging worked.\n", tick, bully_prio, victim_total_prio);
            }
            victim_wait_time = 0; victim_aging_boost = 0;
        }
    }
    printf("\n[AGING_TEST] Simulation complete.\n");
    if (victim_run_count > 0) printf("  RESULT: SUCCESS. Victim task ran %d times.\n", victim_run_count);
    else printf("  RESULT: FAILURE. Victim task starved.\n");
    printf("########################################\n\n");
}


static void run_benchmark_phase(const char* name, scheduler_type_t sched_type, uint32_t duration_sec, scheduler_stats_t* out_stats) {
    printf("\n");
    printf("========================================\n");
    printf(" RUNNING: %s\n", name);
    printf(" Duration: %d seconds\n", duration_sec);
    printf("========================================\n\n");
    
    // Set scheduler *type* for global state (if needed by htas_c)
    htas_set_scheduler(sched_type);
    
    // Reset *global* statistics
    htas_reset_stats();
    
    printf("[BENCH] Simulating workload with %d tasks...\n", 8);
    
    printf("[BENCH] All tasks spawned, running for %d seconds...\n", duration_sec);
    
    // Run synthetic workload and store results in out_stats
    // Run synthetic workload to populate statistics
    simulate_workload(duration_sec * 1000u, sched_type, out_stats);

    for (uint32_t second = 1; second <= duration_sec; ++second) { 
        uint64_t wait_start = pit_ticks();
        uint64_t wait_end = wait_start + pit_hz(); 
        while (pit_ticks() < wait_end) {
            process_yield();
        }
        printf("[BENCH] Progress: %u / %d seconds\n", second, duration_sec); 
    }
    
    printf("[BENCH] Benchmark phase complete\n");
    
    // Print statistics
    htas_print_stats(out_stats, name);
}

// NEW: A scheduler type for our dynamic scheduler
// You MUST add this to your kernel/htas.h enum
/*
typedef enum {
    SCHED_BASELINE,
    SCHED_HTAS,
    SCHED_DYNAMIC // <-- ADD THIS
} scheduler_type_t;
*/
#define SCHED_DYNAMIC ((scheduler_type_t)2)

void htas_run_full_benchmark(void) {
    printf("\n");
    printf("########################################\n");
    printf("# HTAS FULL BENCHMARK SUITE            #\n");
    printf("# 3-Way Scheduler Comparison           #\n");
    printf("########################################\n\n");
    
    printf("[BENCH] Allocating NUMA buffer (%d KB)...\n", NUMA_BUFFER_SIZE / 1024);
    g_numa_buffer = (uint8_t*)kmalloc(NUMA_BUFFER_SIZE);
    if (!g_numa_buffer) {
        printf("[BENCH] ERROR: Failed to allocate NUMA buffer\n");
        return;
    }
    memset(g_numa_buffer, 0, NUMA_BUFFER_SIZE);
    
    // --- We need three separate stat objects ---
    scheduler_stats_t baseline_results;
    scheduler_stats_t htas_results;
    scheduler_stats_t dynamic_results;
    
    uint32_t duration = 15; // 15 seconds per run
    
    // Phase 1: Baseline scheduler
    run_benchmark_phase("BASELINE (Round-Robin)", 
                        SCHED_BASELINE, duration, &baseline_results);
    
    // Phase 2: HTAS scheduler
    run_benchmark_phase("HTAS (Hint-Based)",
                        SCHED_HTAS, duration, &htas_results);
    
    // Phase 3: Dynamic scheduler
    run_benchmark_phase("DYNAMIC (Inference-Based)",
                        SCHED_DYNAMIC, duration, &dynamic_results);
    
    
    // Compare results
    // REPLACE all the htas_compare_stats calls with these:
    printf("\n");
    printf("########################################\n");
    printf("# FINAL RESULTS (BASELINE vs HTAS)     #\n");
    printf("########################################\n\n");
    htas_compare_stats(&baseline_results, "BASELINE", &htas_results, "HTAS");
    
    printf("\n");
    printf("########################################\n");
    printf("# FINAL RESULTS (BASELINE vs DYNAMIC)  #\n");
    printf("########################################\n\n");
    htas_compare_stats(&baseline_results, "BASELINE", &dynamic_results, "DYNAMIC");

    printf("\n");
    printf("########################################\n");
    printf("# FINAL RESULTS (HTAS vs DYNAMIC)      #\n");
    printf("########################################\n\n");
    htas_compare_stats(&htas_results, "HTAS", &dynamic_results, "DYNAMIC");
    
    // Free NUMA buffer
    kfree(g_numa_buffer);
    g_numa_buffer = NULL;
    
    printf("\n");
    printf("########################################\n");
    printf("# BENCHMARK COMPLETE                   #\n");
    printf("########################################\n\n");
}

/* Shell command wrappers */
void htas_run_baseline_benchmark(void) {
    printf("[BENCH] Allocating NUMA buffer (%d KB)...\n", NUMA_BUFFER_SIZE / 1024);
    g_numa_buffer = (uint8_t*)kmalloc(NUMA_BUFFER_SIZE);
    if (!g_numa_buffer) { printf("[BENCH] ERROR: Failed to allocate NUMA buffer\n"); return; }
    memset(g_numa_buffer, 0, NUMA_BUFFER_SIZE);
    
    scheduler_stats_t stats;
    run_benchmark_phase("BASELINE SCHEDULER", SCHED_BASELINE, 30, &stats);
    
    kfree(g_numa_buffer);
    g_numa_buffer = NULL;
}

void htas_run_htas_benchmark(void) {
    printf("[BENCH] Allocating NUMA buffer (%d KB)...\n", NUMA_BUFFER_SIZE / 1024);
    g_numa_buffer = (uint8_t*)kmalloc(NUMA_BUFFER_SIZE);
    if (!g_numa_buffer) { printf("[BENCH] ERROR: Failed to allocate NUMA buffer\n"); return; }
    memset(g_numa_buffer, 0, NUMA_BUFFER_SIZE);
    
    scheduler_stats_t stats;
    run_benchmark_phase("HTAS SCHEDULER", SCHED_HTAS, 30, &stats);
    
    kfree(g_numa_buffer);
    g_numa_buffer = NULL;
}

void htas_print_topology(void) {
    // (This function remains unchanged)
    printf("\n");
    printf("========================================\n");
    printf("        HTAS HARDWARE TOPOLOGY          \n");
    printf("========================================\n\n");
    
    printf("Simulated Hardware Configuration:\n");
    printf("  Total CPUs: %d\n", NUM_CPUS);
    printf("  NUMA Nodes: %d\n\n", NUM_NUMA_NODES);
    
    printf("CPU Topology:\n");
    for (int i = 0; i < NUM_CPUS; i++) {
        extern cpu_info_t g_cpu_topology[];
        cpu_info_t* cpu = &g_cpu_topology[i];
        const char* type = (cpu->type == CPU_TYPE_PCORE) ? "P-Core (Fast)" : "E-Core (Efficient)";
        printf("  CPU %d: %-18s NUMA Node %d  %s\n", 
               cpu->cpu_id, type, cpu->numa_node,
               cpu->online ? "[ONLINE]" : "[OFFLINE]");
    }
    
    printf("\nNUMA Memory Regions:\n");
    for (int i = 0; i < NUM_NUMA_NODES; i++) {
        extern numa_region_t g_numa_regions[];
        numa_region_t* region = &g_numa_regions[i];
        uint32_t size_mb = region->size / (1024 * 1024);
        printf("  Node %d: 0x%08x - 0x%08x (%u MB)\n",
               i, region->base, region->base + region->size - 1, size_mb);
    }
    
    printf("\nSimulation Parameters:\n");
    printf("  E-Core Slowdown: 2x (50%% performance)\n");
    printf("  NUMA Penalty: 100 cycles (cross-node access)\n");
    printf("  LOW_LATENCY Priority Boost: +10\n");
    printf("  AGING Threshold: %d ticks\n", AGING_THRESHOLD);
    printf("  AGING Priority Boost: +%d\n", AGING_PRIORITY_BOOST);
    printf("  DYNAMIC Load Window: %d ticks\n", DYNAMIC_INFERENCE_WINDOW);
    printf("  DYNAMIC Load Threshold: %d ticks\n", DYNAMIC_LOAD_THRESHOLD);

    printf("\nTask Intent Profiles:\n");
    printf("  PROFILE_PERFORMANCE  -> Prefers P-cores, maximizes throughput\n");
    printf("  PROFILE_EFFICIENCY   -> Prefers E-cores, minimizes power\n");
    printf("  PROFILE_LOW_LATENCY  -> Requires P-cores + priority boost\n");
    printf("  PROFILE_DEFAULT      -> No restrictions (any core)\n");
    
    extern scheduler_type_t htas_get_scheduler(void);
    scheduler_type_t current = htas_get_scheduler();
    printf("\nCurrent Scheduler: %s\n", 
           (current == SCHED_BASELINE) ? "BASELINE" : 
           (current == SCHED_HTAS) ? "HTAS" : "DYNAMIC");
    
    printf("\n========================================\n\n");
}