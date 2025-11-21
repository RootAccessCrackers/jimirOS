#include <kernel/shell.h>
#include <kernel/stdio.h>
#include <kernel/tty.h>
#include <kernel/serial.h>
#include <kernel/keyboard.h>
#include <kernel/pmm.h>
#include <kernel/pit.h>
#include <kernel/system.h>
#include <kernel/kmalloc.h>
#include <kernel/vmm.h>
#include <kernel/htas.h>
#include <string.h>
#include <stdint.h>

#define SHELL_BUFSZ 256
#define HIST_MAX 16

static char history[HIST_MAX][SHELL_BUFSZ];
static int hist_len = 0;      /* number of entries valid */
static int hist_head = 0;     /* next insert index */

static void hist_add(const char* line) {
    if (!line || !*line) return;
    int last = (hist_head + HIST_MAX - 1) % HIST_MAX;
    /* avoid duplicate of last */
    int same = 1; const char* a = history[last]; const char* b = line;
    while (*a || *b) { if (*a != *b) { same = 0; break; } if (*a) a++; if (*b) b++; }
    if (hist_len && same) return;
    /* copy */
    char* dst = history[hist_head];
    int i=0; while (line[i] && i < SHELL_BUFSZ-1) { dst[i]=line[i]; i++; }
    dst[i]=0;
    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_len < HIST_MAX) hist_len++;
}

static const char* hist_get(int idx_from_newest) {
    if (idx_from_newest < 0 || idx_from_newest >= hist_len) return 0;
    int pos = (hist_head + HIST_MAX - 1 - idx_from_newest) % HIST_MAX;
    return history[pos];
}

/* Redraw prompt on VGA only (used during interactive editing redraws). */
static void prompt_vga_only(void) {
    terminal_putchar('\r');
    terminal_writestring("jimirOS> ");
}

/* One-time prompt to both VGA and serial (start of a new command). */
static void prompt_both(void) {
    terminal_putchar('\r');
    serial_putchar('\r');
    terminal_writestring("jimirOS> ");
    serial_writestring("jimirOS> ");
}

static int kstrcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int readline(char* buf, size_t cap) {
    size_t len = 0;      /* current line length */
    size_t cur = 0;      /* cursor position within [0..len] */
    int hist_idx = -1;   /* -1 = editing current, 0 = newest history, ... */
    buf[0] = 0;

    while (1) {
        int key = -1;
        int from_serial = 0;
        const int scroll_step = 20;
        
        // Try PS/2 keyboard (USB keyboard injects into PS/2 buffer)
        key = kbd_getch();
        
        // Fall back to serial console
        if (key < 0) {
            int c = serial_getchar();
            if (c >= 0) {
                if (c == '\r') c = '\n';
                if (c == 0x7F) c = '\b';
                from_serial = 1;
                key = c;
            }
        }
        
        if (key < 0) { __asm__ volatile("hlt"); continue; }

    if (key == KEY_PAGE_UP) { terminal_scroll_view(scroll_step); continue; }
    if (key == KEY_PAGE_DOWN) { terminal_scroll_view(-scroll_step); continue; }
    if (key == KEY_SCROLL_UP) { terminal_scroll_view(1); continue; }
    if (key == KEY_SCROLL_DOWN) { terminal_scroll_view(-1); continue; }

        if (terminal_get_scroll_offset() > 0) {
            terminal_scroll_to_bottom();
        }

        if (key == '\n') { /* commit */
            terminal_putchar('\n');
            if (from_serial) serial_putchar('\n');
            buf[len] = 0;
            return (int)len;
        }

        if (key == '\b') { /* backspace */
            if (cur > 0) {
                for (size_t i = cur-1; i+1 <= len; ++i) buf[i] = buf[i+1];
                cur--; len--;
                prompt_vga_only();
                for (size_t i=0;i<len;i++) terminal_putchar(buf[i]);
                terminal_putchar(' ');
                size_t to_move = (len + 1) - cur;
                while (to_move--) terminal_putchar('\b');
                if (from_serial) {
                    serial_putchar('\b');
                    serial_putchar(' ');
                    serial_putchar('\b');
                }
            }
            continue;
        }

        if (key >= 0x20 && key < 0x7F) { /* printable */
            if (len + 1 < cap) {
                for (size_t i = len+1; i-- > cur+1;) buf[i] = buf[i-1];
                buf[cur] = (char)key;
                cur++; len++;
                prompt_vga_only();
                for (size_t i=0;i<len;i++) terminal_putchar(buf[i]);
                size_t to_move = len - cur;
                while (to_move--) terminal_putchar('\b');
                if (from_serial) serial_putchar((char)key);
            }
            continue;
        }

        if (key == KEY_LEFT) { if (cur > 0) { terminal_putchar('\b'); cur--; } continue; }
        if (key == KEY_RIGHT){ if (cur < len) { terminal_putchar(buf[cur]); cur++; } continue; }
    if (key == KEY_HOME) { prompt_vga_only(); cur = 0; for (size_t i=0;i<len;i++) terminal_putchar(buf[i]); size_t to_move=len; while (to_move--) terminal_putchar('\b'); continue; }
        if (key == KEY_END)  { while (cur < len) { terminal_putchar(buf[cur]); cur++; } continue; }
        if (key == KEY_DELETE) {
            if (cur < len) {
                for (size_t i = cur; i+1 <= len; ++i) buf[i] = buf[i+1];
                len--;
                prompt_vga_only();
                for (size_t i=0;i<len;i++) terminal_putchar(buf[i]);
                terminal_putchar(' ');
                size_t to_move = (len + 1) - cur;
                while (to_move--) terminal_putchar('\b');
            }
            continue;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            int new_idx = hist_idx;
            if (key == KEY_UP) {
                if (hist_idx + 1 < hist_len) new_idx = hist_idx + 1;
            } else {
                if (hist_idx >= 0) new_idx = hist_idx - 1;
            }
            if (new_idx != hist_idx) {
                hist_idx = new_idx;
                const char* h = (hist_idx >= 0) ? hist_get(hist_idx) : "";
                size_t i=0; while (h[i] && i<cap-1) { buf[i]=h[i]; i++; } buf[i]=0;
                len = i; cur = len;
                prompt_vga_only();
                for (size_t j=0;j<len;j++) terminal_putchar(buf[j]);
            }
            continue;
        }
    }
}

static void cmd_help(void) {
    printf("Built-ins:\n");
    printf("  help         - show this help\n");
    printf("  about        - kernel info\n");
    printf("  banner       - show banner\n");
    printf("  clear        - clear screen\n");
    printf("  echo ARG     - print ARG\n");
    printf("  mem          - show memory stats\n");
    printf("  frames       - show PMM frames\n");
    printf("  uptime       - show ticks and seconds\n");
    printf("  map ADDR     - show phys mapping\n");
    printf("  peek ADDR    - read u32 at ADDR\n");
    printf("  poke ADDR VAL- write u32 VAL to ADDR\n");
    printf("  hexdump ADDR [LEN] - hex dump\n");
    printf("  halt         - stop CPU\n");
    printf("  reboot       - reset machine\n");
    printf("  userdemo     - run built-in ring3 demo\n");
    printf("  runmod       - load and run first multiboot module (ELF)\n");
    printf("  exec NAME    - run module by name (ELF)\n");
    printf("  ls           - list files\n");
    printf("  cat NAME     - dump a file\n");
    printf("  ps           - list kernel threads\n");
    printf("  spawn        - create a demo thread\n");
    printf("  kdbg         - enter kernel debugger\n");
    printf("\n");
    printf("HTAS Scheduler (Thesis Research):\n");
    printf("  htas         - show CPU topology and NUMA configuration\n");
    printf("  htas-baseline- run 30s benchmark with BASELINE (round-robin)\n");
    printf("  htas-test    - run 30s benchmark with HTAS (topology-aware)\n");
    printf("  htas-full    - run FULL comparison (both schedulers back-to-back)\n");
    printf("  htas-stats   - show current scheduler statistics\n");
    printf("  sched TYPE   - switch scheduler (baseline, htas, dynamic)\n");
}

static void cmd_clear(void) { terminal_clear(); }

static void cmd_echo(const char* s) { printf("%s\n", s); }

static void cmd_mem(void) {
    uint32_t tot = pmm_total_frames();
    uint32_t fre = pmm_free_frames();
    uint32_t used = (tot >= fre) ? (tot - fre) : 0;
    uint32_t mib = (tot * 4096u) / (1024u*1024u);
    uint32_t free_pct = (tot ? (fre * 100u) / tot : 0u);
    uint32_t used_pct = (tot ? 100u - free_pct : 0u);
    printf("PMM: total=%u (%u MiB) free=%u (%u%%) used=%u (%u%%)\n",
           tot, mib, fre, free_pct, used, used_pct);
}

static void cmd_uptime(void) {
    uint64_t t = pit_ticks();
    uint32_t hz = pit_hz();
    uint32_t ticks32 = (uint32_t)(t & 0xFFFFFFFFu);
    uint32_t secs32 = (hz ? (uint32_t)(t / hz) : 0u);
    printf("uptime: ticks=%u (hz=%u) ~ %u s\n", ticks32, hz, secs32);
}

static void cmd_banner(void) {
    printf("============================\n");
    printf("  jimirOS kernel shell\n");
    printf("============================\n");
}

static void cmd_about(void) {
    extern uint32_t kernel_phys_start, kernel_phys_end;
    printf("kernel phys: %x..%x\n", &kernel_phys_start, &kernel_phys_end);
}

static int parse_u32(const char* s, uint32_t* out) {
    uint32_t base = 10; if (s[0]=='0' && (s[1]=='x' || s[1]=='X')) { base=16; s+=2; }
    uint32_t v=0; if (!*s) return 0;
    while (*s) {
        char c=*s++; uint32_t d;
        if (c>='0'&&c<='9') d=c-'0';
        else if (c>='a'&&c<='f') d=10+c-'a';
        else if (c>='A'&&c<='F') d=10+c-'A';
        else return 0;
        if (d>=base) return 0;
        v = v*base + d;
    }
    *out=v; return 1;
}

static void cmd_map(const char* s) {
    uint32_t va; if (!parse_u32(s, &va)) { printf("usage: map ADDR\n"); return; }
    uint32_t pa = vmm_resolve(va);
    if (pa) printf("%x -> %x\n", va, pa); else printf("%x not mapped\n", va);
}

static void cmd_peek(const char* s) {
    uint32_t va; if (!parse_u32(s, &va)) { printf("usage: peek ADDR\n"); return; }
    uint32_t val = *(volatile uint32_t*)va;
    printf("[%x] = %x\n", va, val);
}

static void cmd_poke(const char* s1, const char* s2) {
    uint32_t va, val; if (!parse_u32(s1, &va) || !parse_u32(s2, &val)) { printf("usage: poke ADDR VAL\n"); return; }
    *(volatile uint32_t*)va = val;
    printf("[%x] <- %x\n", va, val);
}

static void cmd_hexdump(const char* s1, const char* s2) {
    uint32_t va; if (!parse_u32(s1, &va)) { printf("usage: hexdump ADDR [LEN]\n"); return; }
    uint32_t len = 64; if (s2 && *s2) { uint32_t l; if (parse_u32(s2, &l)) len = l; }
    uint8_t* p = (uint8_t*)va;
    for (uint32_t i=0; i<len; i+=16) {
        printf("%08x: ", va+i);
        for (uint32_t j=0;j<16 && i+j<len;j++) printf("%02x ", p[i+j]);
        printf("\n");
    }
}

/* forward decls for extended commands */
extern void userdemo_run(void);
extern int  elf_run_first_module(void);
extern int  elf_run_module_by_name(const char* name);
extern void fs_list_print(void);
extern int  fs_open(const char* name);
extern int  fs_read(int fd, void* buf, unsigned len);
extern int  fs_close(int fd);

static void execute(char* line) {
    /* trim leading spaces */
    while (*line == ' ') line++;
    if (!*line) return;    /* simple split into cmd and rest */
    char* arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg++ = 0; while (*arg == ' ') arg++; }

    if (!kstrcmp(line, "help")) { cmd_help(); return; }
    if (!kstrcmp(line, "about")) { cmd_about(); return; }
    if (!kstrcmp(line, "banner")) { cmd_banner(); return; }
    if (!kstrcmp(line, "clear")) { cmd_clear(); return; }
    if (!kstrcmp(line, "echo")) { cmd_echo(arg ? arg : ""); return; }
    if (!kstrcmp(line, "mem")) { cmd_mem(); return; }
    if (!kstrcmp(line, "frames")) { cmd_mem(); return; }
    if (!kstrcmp(line, "uptime")) { cmd_uptime(); return; }
    if (!kstrcmp(line, "map")) { if (arg) cmd_map(arg); else printf("usage: map ADDR\n"); return; }
    if (!kstrcmp(line, "peek")) { if (arg) cmd_peek(arg); else printf("usage: peek ADDR\n"); return; }
    if (!kstrcmp(line, "poke")) {
        const char* a2 = arg; while (a2 && *a2 && *a2!=' ') a2++;
        if (a2 && *a2==' ') { *(char*)a2=0; a2++; while (*a2==' ') a2++; cmd_poke(arg, a2); }
        else printf("usage: poke ADDR VAL\n");
        return;
    }
    if (!kstrcmp(line, "hexdump")) {
        const char* a2 = arg; while (a2 && *a2 && *a2!=' ') a2++;
        const char* a3 = 0;
        if (a2 && *a2==' ') { *(char*)a2=0; a2++; while (*a2==' ') a2++; a3=a2; while (a3 && *a3 && *a3!=' ') a3++; if (a3 && *a3==' ') { *(char*)a3=0; a3++; while (*a3==' ') a3++; } }
        if (arg) cmd_hexdump(arg, a3); else printf("usage: hexdump ADDR [LEN]\n");
        return;
    }
    if (!kstrcmp(line, "halt")) { printf("halting...\n"); cpu_halt(); return; }
    if (!kstrcmp(line, "reboot")) { printf("rebooting...\n"); cpu_reboot(); return; }
    if (!kstrcmp(line, "userdemo")) { userdemo_run(); return; }
    if (!kstrcmp(line, "runmod")) { int rc = elf_run_first_module(); printf("runmod rc=%d\n", rc); return; }
    if (!kstrcmp(line, "exec")) {
        extern int elf_run_from_filesystem(const char* path);
        if (!arg || !*arg) { printf("usage: exec NAME\n"); return; }
        
        /* Try to load from filesystem first */
        int rc = elf_run_from_filesystem(arg);
        if (rc == -1) {
            /* File not found in filesystem, try modules */
            printf("Not found in filesystem, trying modules...\n");
            rc = elf_run_module_by_name(arg);
        }
        
        if (rc != 0) {
            printf("exec failed: rc=%d\n", rc);
        }
        return;
    }
    if (!kstrcmp(line, "ls")) { fs_list_print(); return; }
    if (!kstrcmp(line, "ps")) { extern void sched_ps(void); sched_ps(); return; }
    if (!kstrcmp(line, "spawn")) {
        extern int kthread_create(void (*fn)(void*), void*, const char*);
        void demo(void* _){ for(;;){ printf("[thr] tick\n"); for(volatile int i=0;i<1000000;i++); } }
        int id = kthread_create(demo, 0, "demo");
        if (id<0) printf("spawn failed\n"); else printf("spawned thread %d\n", id);
        return;
    }
    if (!kstrcmp(line, "kdbg")) { extern void kdbg_enter(void); kdbg_enter(); return; }
    
    /* HTAS Scheduler Commands */
    if (!kstrcmp(line, "htas")) {
        extern void htas_print_topology(void);
        htas_print_topology();
        return;
    }
    if (!kstrcmp(line, "htas-baseline")) {
        printf("\n=== Running BASELINE (Round-Robin) Benchmark ===\n");
        extern void htas_run_baseline_benchmark(void);
        htas_run_baseline_benchmark();
        return;
    }
    if (!kstrcmp(line, "htas-test")) {
        printf("\n=== Running HTAS (Topology-Aware) Benchmark ===\n");
        extern void htas_run_htas_benchmark(void);
        htas_run_htas_benchmark();
        return;
    }
    if (!kstrcmp(line, "htas-full")) {
        printf("\n=== Running FULL COMPARISON Benchmark ===\n");
        extern void htas_run_full_benchmark(void);
        htas_run_full_benchmark();
        return;
    }
    if (!kstrcmp(line, "htas-stats")) {
        extern scheduler_stats_t* htas_get_stats(void);
        extern void htas_print_stats(scheduler_stats_t*, const char*);
        extern scheduler_type_t htas_get_scheduler(void);
        scheduler_stats_t* stats = htas_get_stats();
        const char* name = (htas_get_scheduler() == 0) ? "BASELINE" : "HTAS";
        htas_print_stats(stats, name);
        return;
    }
    if (!kstrcmp(line, "sched")) {
        if (!arg || !*arg) {
            printf("usage: sched TYPE (baseline, htas, dynamic)\n");
            return;
        }
        extern void htas_set_scheduler(scheduler_type_t);
        if (!kstrcmp(arg, "baseline")) {
            htas_set_scheduler(0); // SCHED_BASELINE
        } else if (!kstrcmp(arg, "htas")) {
            htas_set_scheduler(1); // SCHED_HTAS
        } else if (!kstrcmp(arg, "dynamic")) {
            htas_set_scheduler(2); // SCHED_DYNAMIC
        } else {
            printf("unknown scheduler type: %s\n", arg);
        }
        return;
    }
    
    if (!kstrcmp(line, "cat")) {
        if (!arg || !*arg) { printf("usage: cat NAME\n"); return; }
        int fd = fs_open(arg);
        if (fd < 0) { printf("cat: not found: %s\n", arg); return; }
        char buf[256];
        while (1) {
            int n = fs_read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            for (int i=0;i<n;i++) terminal_putchar(buf[i]);
        }
        fs_close(fd);
        terminal_putchar('\n');
        return;
    }

    printf("Unknown command: %s (try 'help')\n", line);
}

void shell_run(void) {
    char buf[SHELL_BUFSZ];
    printf("Welcome to jimirOS shell. Type 'help'.\n");
    while (1) {
        /* Ensure interrupts are enabled before waiting for keyboard input.
           Syscalls may have left IF=0 due to the int gate and our hard-switch
           exit path. */
        __asm__ volatile("sti" ::: "memory");
        prompt_both();
        int n = readline(buf, sizeof(buf));
        if (n >= 0) { hist_add(buf); execute(buf); }
    }
}
