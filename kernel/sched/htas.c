#include <kernel/htas.h>
#include <kernel/process.h>
#include <kernel/tty.h>
#include <kernel/kmalloc.h>
#include <kernel/stdio.h>
#include <string.h>

cpu_info_t g_cpu_topology[NUM_CPUS] = {
    { .cpu_id = 0, .type = CPU_TYPE_PCORE, .numa_node = 0, .online = true },
    { .cpu_id = 1, .type = CPU_TYPE_PCORE, .numa_node = 0, .online = true },
    { .cpu_id = 2, .type = CPU_TYPE_ECORE, .numa_node = 1, .online = true },
    { .cpu_id = 3, .type = CPU_TYPE_ECORE, .numa_node = 1, .online = true },
};

numa_region_t g_numa_regions[NUM_NUMA_NODES] = {
    { .base = 0x00000000, .size = 0x08000000 },
    { .base = 0x08000000, .size = 0x08000000 },
};

static scheduler_type_t g_current_scheduler = SCHED_BASELINE;
static uint8_t g_current_cpu = 0;
static uint64_t g_tick_counter = 0;

static scheduler_stats_t* active_stats(void) {
    return (g_current_scheduler == SCHED_BASELINE)
            ? &g_baseline_stats : &g_htas_stats;
}

scheduler_stats_t g_baseline_stats;
scheduler_stats_t g_htas_stats;

void htas_init(void) {
    printf("hint-BASED Topology-Aware Scheduler SIMULATOR caus i suck at x64\n");
    printf("[HTAS] Topology Map:\n");
    
    for (int i = 0; i < NUM_CPUS; i++) {
        const char* type_str = (g_cpu_topology[i].type == CPU_TYPE_PCORE) ? "P-Core" : "E-Core";
        printf("[HTAS]   CPU %d: %s, NUMA Node %d\n", 
               i, type_str, g_cpu_topology[i].numa_node);
    }
    
    printf("[HTAS] NUMA Regions:\n");
    for (int i = 0; i < NUM_NUMA_NODES; i++) {
        printf("[HTAS]   Node %d: 0x%08x - 0x%08x\n",
               i, g_numa_regions[i].base, 
               g_numa_regions[i].base + g_numa_regions[i].size);
    }
    
    memset(&g_baseline_stats, 0, sizeof(scheduler_stats_t));
    memset(&g_htas_stats, 0, sizeof(scheduler_stats_t));
    
    g_current_scheduler = SCHED_BASELINE;
    printf("[HTAS] Active scheduler: BASELINE (Round-Robin)\n");
}

void htas_set_scheduler(scheduler_type_t type) {
    g_current_scheduler = type;
    
    const char* name = "UNKNOWN";
    switch (type) {
        case SCHED_BASELINE:
            name = "BASELINE";
            break;
        case SCHED_HTAS:
            name = "HTAS";
            break;
        case SCHED_DYNAMIC:
            name = "DYNAMIC";
            break;
    }
    
    printf("[HTAS] Switched to %s scheduler\n", name);
}

scheduler_type_t htas_get_scheduler(void) {
    return g_current_scheduler;
}

cpu_type_t htas_get_cpu_type(uint8_t cpu_id) {
    if (cpu_id >= NUM_CPUS) return CPU_TYPE_PCORE;
    return g_cpu_topology[cpu_id].type;
}

uint8_t htas_get_numa_node_for_cpu(uint8_t cpu_id) {
    if (cpu_id >= NUM_CPUS) return 0;
    return g_cpu_topology[cpu_id].numa_node;
}

uint8_t htas_get_numa_node_for_address(void* addr) {
    uint32_t address = (uint32_t)addr;
    
    for (int i = 0; i < NUM_NUMA_NODES; i++) {
        uint32_t base = g_numa_regions[i].base;
        uint32_t end = base + g_numa_regions[i].size;
        
        if (address >= base && address < end) {
            return i;
        }
    }
    
    return 0;
}

uint32_t htas_calculate_affinity(const task_profile_t* profile) {
    uint32_t mask = 0;
    
    switch (profile->intent) {
        case PROFILE_PERFORMANCE:
        case PROFILE_LOW_LATENCY:
            for (int i = 0; i < NUM_CPUS; i++) {
                if (g_cpu_topology[i].type == CPU_TYPE_PCORE) {
                    mask |= (1 << i);
                }
            }
            break;
            
        case PROFILE_EFFICIENCY:
            for (int i = 0; i < NUM_CPUS; i++) {
                if (g_cpu_topology[i].type == CPU_TYPE_ECORE) {
                    mask |= (1 << i);
                }
            }
            break;
            
        case PROFILE_DEFAULT:
            mask = (1 << NUM_CPUS) - 1;
            break;
    }
    
    if (profile->primary_data_region != NULL) {
        uint8_t numa_node = htas_get_numa_node_for_address(profile->primary_data_region);
        uint32_t numa_mask = 0;
        
        for (int i = 0; i < NUM_CPUS; i++) {
            if (g_cpu_topology[i].numa_node == numa_node) {
                numa_mask |= (1 << i);
            }
        }
        
        mask &= numa_mask;
        
        if (mask == 0) {
            printf("[HTAS] WARNING: NUMA restriction eliminated all CPUs, using intent mask\n");
            return htas_calculate_affinity(&(task_profile_t){
                .intent = profile->intent,
                .primary_data_region = NULL
            });
        }
    }
    
    return mask;
}

bool htas_can_run_on_cpu(struct process* proc, uint8_t cpu_id) {
    if (cpu_id >= NUM_CPUS) return false;
    if (!proc->htas_info) return true;
    
    uint32_t mask = proc->htas_info->cpu_affinity_mask;
    return (mask & (1 << cpu_id)) != 0;
}

int sys_sched_set_profile(uint32_t pid, const task_profile_t* profile) {
    process_t* proc = process_find(pid);
    if (!proc) {
        printf("[HTAS] sys_sched_set_profile: PID %d not found\n", pid);
        return -1;
    }
    
    // Allocate HTAS info if not present
    if (!proc->htas_info) {
        proc->htas_info = kmalloc(sizeof(htas_task_info_t));
        if (!proc->htas_info) {
            printf("[HTAS] sys_sched_set_profile: Out of memory\n");
            return -1;
        }
        memset(proc->htas_info, 0, sizeof(htas_task_info_t));
    }
    
    // Copy profile
    memcpy(&proc->htas_info->profile, profile, sizeof(task_profile_t));
    
    // Calculate CPU affinity mask
    proc->htas_info->cpu_affinity_mask = htas_calculate_affinity(profile);
    
    // Set priority boost for LOW_LATENCY tasks
    if (profile->intent == PROFILE_LOW_LATENCY) {
        proc->htas_info->priority_boost = LOW_LATENCY_PRIORITY_BOOST;
    } else {
        proc->htas_info->priority_boost = 0;
    }
    
    // Calculate preferred NUMA node
    if (profile->primary_data_region != NULL) {
        proc->htas_info->preferred_numa_node = 
            htas_get_numa_node_for_address(profile->primary_data_region);
    } else {
        proc->htas_info->preferred_numa_node = 0;
    }
    
    const char* intent_name[] = {"PERFORMANCE", "EFFICIENCY", "LOW_LATENCY", "DEFAULT"};
    printf("[HTAS] PID %d set profile: %s, affinity=0x%x, NUMA node=%d\n",
           pid, intent_name[profile->intent], 
           proc->htas_info->cpu_affinity_mask,
           proc->htas_info->preferred_numa_node);
    
    return 0;
}

/* ============================================================================
 * E-CORE SLOWDOWN SIMULATION
 * ============================================================================ */

static void simulate_ecore_slowdown(uint8_t cpu_id) {
    if (htas_get_cpu_type(cpu_id) == CPU_TYPE_ECORE) {
        // E-core: Burn extra cycles to simulate slowdown
        // In reality, we'd give half the time slice or slow the tick rate
        for (volatile int i = 0; i < ECORE_SLOWDOWN_FACTOR * 1000; i++);
    }
}

/* ============================================================================
 * NUMA PENALTY SIMULATION
 * ============================================================================ */

void htas_simulate_memory_access(struct process* proc, void* addr, uint32_t size) {
    if (!proc || !proc->htas_info) return;
    
    uint8_t memory_numa = htas_get_numa_node_for_address(addr);
    uint8_t cpu_numa = htas_get_numa_node_for_cpu(g_current_cpu);
    
    if (memory_numa != cpu_numa) {
        for (volatile int i = 0; i < NUMA_PENALTY_CYCLES; i++);
        
        proc->htas_info->numa_penalties++;
        
        scheduler_stats_t* stats = (g_current_scheduler == SCHED_BASELINE) 
                                    ? &g_baseline_stats : &g_htas_stats;
        stats->numa_penalties++;
    }
}

struct process* baseline_select_next(void) {
    static int rr_cursor = -1;

    process_t* processes = process_get_list();
    process_t* current = process_current();
    process_t* current_candidate = NULL;

    int current_idx = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (&processes[i] == current) {
            current_idx = i;
            break;
        }
    }

    if (current && (current->state == PROC_READY || current->state == PROC_RUNNING)) {
        current_candidate = current;
    }

    if (current_idx >= 0) {
        rr_cursor = current_idx;
    }

    int start = (rr_cursor >= 0) ? ((rr_cursor + 1) % MAX_PROCESSES) : 0;

    for (int scanned = 0; scanned < MAX_PROCESSES; scanned++) {
        int idx = (start + scanned) % MAX_PROCESSES;
        process_t* proc = &processes[idx];

        if (proc->state == PROC_UNUSED) continue;
        if (proc == current_candidate) continue;
        if (proc->state != PROC_READY && proc->state != PROC_RUNNING) continue;

        rr_cursor = idx;
        return proc;
    }

    if (current_candidate) {
        rr_cursor = (current_idx >= 0) ? current_idx : rr_cursor;
        return current_candidate;
    }

    rr_cursor = -1;
    return NULL;
}

struct process* htas_select_next(uint8_t cpu_id) {
    process_t* best = NULL;
    int best_priority = -1000;
    
    process_t* processes = process_get_list();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* proc = &processes[i];
        
        if (proc->state != PROC_READY && proc->state != PROC_RUNNING) {
            continue;
        }
        
        if (!htas_can_run_on_cpu(proc, cpu_id)) {
            continue;
        }
        
        int priority = 0;
        
        if (proc->htas_info) {
            // Add boost from task intent (e.g., LOW_LATENCY)
            priority += proc->htas_info->priority_boost;
            
            // *** NEW: Add boost from priority aging ***
            priority += proc->htas_info->priority_boost_aging;
            
            // Add boost for NUMA locality
            uint8_t cpu_numa = htas_get_numa_node_for_cpu(cpu_id);
            if (proc->htas_info->preferred_numa_node == cpu_numa) {
                priority += 5;
            }
        }
        
        if (priority > best_priority || !best) {
            best_priority = priority;
            best = proc;
        }
    }
    
    return best;
}

struct process* htas_pick_next_process(struct process* current) {
    g_tick_counter++;

    scheduler_stats_t* stats = active_stats();
    stats->total_ticks++;

    process_t* next = NULL;

    // 1. Select the next process to run
    if (g_current_scheduler == SCHED_BASELINE) {
        next = baseline_select_next();
    } else {
        next = htas_select_next(g_current_cpu);
    }

    if (!next) {
        return current; // No runnable processes found
    }

    // --- NEW: PRIORITY AGING LOOP ---
    // 2. Age all other ready tasks that were *not* selected
    if (g_current_scheduler == SCHED_HTAS) {
        process_t* processes = process_get_list();
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t* proc = &processes[i];

            // Check if task is ready, has HTAS info, and is NOT the one we just picked
            if (proc->state == PROC_READY && proc != next && proc->htas_info) {
                
                proc->htas_info->wait_time++;
                
                // If it's waited too long, give it a boost
                // (Assumes AGING_THRESHOLD is defined in htas.h)
                if (proc->htas_info->wait_time > AGING_THRESHOLD) { 
                    // (Assumes AGING_PRIORITY_BOOST is defined in htas.h)
                    proc->htas_info->priority_boost_aging = AGING_PRIORITY_BOOST; 
                }
            }
        }
    }
    // --- END NEW ---

    // 3. Return the selected process
    return next;
}

void htas_record_switch(process_t* current, process_t* next) {
    if (!next || next == current) {
        return;
    }

    scheduler_stats_t* stats = active_stats();
    stats->context_switches++;

    if (next->htas_info) {
        // --- NEW: RESET AGING COUNTERS ---
        // The task is now running, so reset its wait time and aging boost
        next->htas_info->wait_time = 0;
        next->htas_info->priority_boost_aging = 0;
        // --- END NEW ---

        next->htas_info->total_switches++;
        task_intent_t intent = next->htas_info->profile.intent;
        if ((int)intent < 0 || intent > PROFILE_DEFAULT) {
            intent = PROFILE_DEFAULT;
        }
        stats->intent_stats[intent].switches++;
    }

    simulate_ecore_slowdown(g_current_cpu);

    if (htas_get_cpu_type(g_current_cpu) == CPU_TYPE_PCORE) {
        stats->total_power_consumption += 100;
        stats->pcore_time_us += 10;
    } else {
        stats->total_power_consumption += 40;
        stats->ecore_time_us += 10;
    }

    g_current_cpu = (g_current_cpu + 1) % NUM_CPUS;
}

/* ============================================================================
 * STATISTICS
 * ============================================================================ */

scheduler_stats_t* htas_get_stats(void) {
    return (g_current_scheduler == SCHED_BASELINE) 
            ? &g_baseline_stats : &g_htas_stats;
}

void htas_reset_stats(void) {
    memset(&g_baseline_stats, 0, sizeof(scheduler_stats_t));
    memset(&g_htas_stats, 0, sizeof(scheduler_stats_t));
    printf("[HTAS] Statistics reset\n");
}

void htas_print_stats(scheduler_stats_t* stats, const char* name) {
    printf("\n========================================\n");
    printf(" %s SCHEDULER STATISTICS\n", name);
    printf("========================================\n");
    
    printf("Total ticks:           %u\n", (uint32_t)stats->total_ticks);
    printf("Context switches:      %u\n", (uint32_t)stats->context_switches);
    printf("NUMA penalties:        %u\n", (uint32_t)stats->numa_penalties);
    printf("P-core time:           %u us\n", (uint32_t)stats->pcore_time_us);
    printf("E-core time:           %u us\n", (uint32_t)stats->ecore_time_us);
    printf("Power consumption:     %u units\n", (uint32_t)stats->total_power_consumption);
    
    printf("\nPer-Intent Statistics:\n");
    const char* intent_names[] = {"PERFORMANCE", "EFFICIENCY", "LOW_LATENCY", "DEFAULT"};
    
    for (int i = 0; i < 4; i++) {
        if (stats->intent_stats[i].switches > 0) {
            printf("  %s:\n", intent_names[i]);
            printf("    Runtime:      %u us\n", (uint32_t)stats->intent_stats[i].runtime_us);
            printf("    Switches:     %u\n", (uint32_t)stats->intent_stats[i].switches);
            
            if (i == PROFILE_LOW_LATENCY) {
                printf("    Avg Latency:  %u us\n", (uint32_t)stats->intent_stats[i].avg_latency_us);
                printf("    Max Jitter:   %u us\n", (uint32_t)stats->intent_stats[i].max_jitter_us);
            }
        }
    }
    
    printf("========================================\n\n");
}

/* Find the htas_compare_stats function and REPLACE it with this new version: */

void htas_compare_stats(scheduler_stats_t* stats_a, const char* name_a, 
                        scheduler_stats_t* stats_b, const char* name_b) 
{
    printf("\n========================================\n");
    printf(" %s vs %s COMPARISON\n", name_a, name_b);
    printf("========================================\n");
    
    // NUMA penalties
    int32_t numa_diff = ((int32_t)stats_a->numa_penalties - (int32_t)stats_b->numa_penalties);
    int numa_improvement = (stats_a->numa_penalties > 0) 
        ? (numa_diff * 100) / (int32_t)stats_a->numa_penalties : 0;
    printf("NUMA Penalties:\n");
    printf("  %s: %u\n", name_a, (uint32_t)stats_a->numa_penalties);
    printf("  %s: %u\n", name_b, (uint32_t)stats_b->numa_penalties);
    printf("  %s Improvement: %d%% reduction\n", name_b, numa_improvement);
    
    // Power consumption
    int32_t power_diff = ((int32_t)stats_a->total_power_consumption - (int32_t)stats_b->total_power_consumption);
    int power_improvement = (stats_a->total_power_consumption > 0)
        ? (power_diff * 100) / (int32_t)stats_a->total_power_consumption : 0;
    printf("\nPower Consumption:\n");
    printf("  %s: %u units\n", name_a, (uint32_t)stats_a->total_power_consumption);
    printf("  %s: %u units\n", name_b, (uint32_t)stats_b->total_power_consumption);
    printf("  %s Improvement: %d%% reduction\n", name_b, power_improvement);
    
    // Context switches
    printf("\nContext Switches:\n");
    printf("  %s: %u\n", name_a, (uint32_t)stats_a->context_switches);
    printf("  %s: %u\n", name_b, (uint32_t)stats_b->context_switches);
    
    // LOW_LATENCY jitter comparison
    printf("\nLOW_LATENCY Task Performance:\n");
    printf("  %s Max Jitter: %u us\n", 
           name_a, (uint32_t)stats_a->intent_stats[PROFILE_LOW_LATENCY].max_jitter_us);
    printf("  %s Max Jitter: %u us\n",
           name_b, (uint32_t)stats_b->intent_stats[PROFILE_LOW_LATENCY].max_jitter_us);
    
    printf("========================================\n\n");
}