#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>

typedef enum {ALG_FCFS, ALG_SJF, ALG_RR, ALG_PRIORITY} alg_t;

typedef struct {
    char *pid; // process identifier
    int arrival; // arrival time (cycles)
    int burst; // total CPU time required
    int remaining; // remaining CPU time
    int priority; // lower = higher priority
    sem_t *sem; // per-process named semaphore (sem_open)
    char sem_name[64]; // name used with sem_open (must start with '/')
    pthread_t thread;
    int started; // boolean: has started?
    int start_time; // first time dispatched
    int finish_time; // finish time
    int admitted; // has been added to READY queue at least once
    // for internal synchronization
    pthread_mutex_t lock;
} process_t;

typedef struct {
    int start;
    int end; // exclusive
    char *pid;
} gantt_seg_t;

// dynamic arrays
process_t *processes = NULL;
int proc_count = 0;
int capacity = 0;

// READY queue: dynamic array of indices into processes[]
int *ready = NULL;
int ready_count = 0;
int ready_cap = 0;
pthread_mutex_t ready_lock = PTHREAD_MUTEX_INITIALIZER;

// global simulation control
int current_time = 0;
alg_t algorithm = ALG_FCFS;
int quantum = 1; // for RR; default 1
int total_finished = 0;
int cpu_busy_cycles = 0;

// for Gantt
gantt_seg_t *gantt = NULL;
int gantt_count = 0;
int gantt_cap = 0;

void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

// ready queue helpers
void ready_ensure_capacity() {
    if (!ready_cap) {
        ready_cap = 8;
        ready = malloc(sizeof(int)*ready_cap);
    } else if (ready_count >= ready_cap) {
        ready_cap *= 2;
        ready = realloc(ready, sizeof(int)*ready_cap);
    }
}

void ready_push_back(int idx) {
    pthread_mutex_lock(&ready_lock);
    ready_ensure_capacity();
    ready[ready_count++] = idx;
    pthread_mutex_unlock(&ready_lock);
}

void ready_push_front(int idx) {
    pthread_mutex_lock(&ready_lock);
    ready_ensure_capacity();
    for (int i = ready_count; i > 0; --i) ready[i] = ready[i-1];
    ready[0] = idx;
    ++ready_count;
    pthread_mutex_unlock(&ready_lock);
}

int ready_pop_front() {
    pthread_mutex_lock(&ready_lock);
    if (ready_count == 0) { pthread_mutex_unlock(&ready_lock); return -1;}
    int idx = ready[0];
    for (int i = 1; i < ready_count; ++i) ready[i-1] = ready[i];
    --ready_count;
    pthread_mutex_unlock(&ready_lock);
    return idx;
}

// remove by index (position)
int ready_remove_pos(int pos) {
    pthread_mutex_lock(&ready_lock);
    if (pos < 0 || pos >= ready_count) { pthread_mutex_unlock(&ready_lock); return -1; }
    int idx = ready[pos];
    for (int i = pos+1; i < ready_count; ++i) ready[i-1] = ready[i];
    --ready_count;
    pthread_mutex_unlock(&ready_lock);
    return idx;
}

// find position of process in ready queue, -1 if not found
int ready_find_pos(int proc_idx) {
    pthread_mutex_lock(&ready_lock);
    for (int i = 0; i < ready_count; ++i) {
        if (ready[i] == proc_idx) { pthread_mutex_unlock(&ready_lock); return i; }
    }
    pthread_mutex_unlock(&ready_lock);
    return -1;
}

// Insert in ready according to SJF or Priority ordering
void ready_insert_ordered(int proc_idx) {
    pthread_mutex_lock(&ready_lock);
    ready_ensure_capacity();
    int i = 0;

    if (algorithm == ALG_SJF) {
        int r = processes[proc_idx].remaining;
        for (i = 0; i < ready_count; ++i) {
            int r2 = processes[ready[i]].remaining;
            if (r2 > r) break;
            // tie-breaker: earlier arrival goes first
            if (r2 == r && processes[ready[i]].arrival > processes[proc_idx].arrival)
                break;
        }
    } else if (algorithm == ALG_PRIORITY) {
        int pr = processes[proc_idx].priority;
        for (i = 0; i < ready_count; ++i) {
            int pr2 = processes[ready[i]].priority;
            if (pr2 > pr) break;
            // tie-breaker: if equal priority, earlier arrival first
            if (pr2 == pr && processes[ready[i]].arrival > processes[proc_idx].arrival)
                break;
        }
    } else {
        i = ready_count; // append for FCFS/RR default
    }

    for (int j = ready_count; j > i; --j)
        ready[j] = ready[j - 1];

    ready[i] = proc_idx;
    ready_count++;
    pthread_mutex_unlock(&ready_lock);
}

// Handles initalization and resizing of gantt array
void gantt_ensure() {
    if (!gantt_cap) { gantt_cap = 8; gantt = malloc(sizeof(gantt_seg_t)*gantt_cap); }
    else if (gantt_count >= gantt_cap) { gantt_cap *= 2; gantt = realloc(gantt, sizeof(gantt_seg_t)*gantt_cap); }
}

// Append a segment to Gantt chart
void gantt_append(int start, int end, const char *pid) {
    if (end <= start) return;
    // if last segment has same pid and no gap, extend it
    if (gantt_count > 0 && strcmp(gantt[gantt_count-1].pid, pid) == 0 && gantt[gantt_count-1].end == start) {
        gantt[gantt_count-1].end = end;
        return;
    }
    gantt_ensure();
    gantt[gantt_count].start = start;
    gantt[gantt_count].end = end;
    gantt[gantt_count].pid = strdup(pid);
    gantt_count++;
}

// Simulates process executing one time unit per scheduler signal, tracking start and finish times
void *process_thread(void *arg) {
    int idx = (intptr_t)arg;
    process_t *p = &processes[idx];

    while (1) {
    // wait for scheduler to permit one cycle
    sem_wait(p->sem);

        pthread_mutex_lock(&p->lock);
        // first time starting
        if (!p->started) {
            p->started = 1;
            p->start_time = current_time; // scheduler posts semaphore at this current_time
        }
        // perform one cycle
        if (p->remaining > 0) {
            p->remaining -= 1;
        }
        // if finished, record finish_time and exit thread
        if (p->remaining == 0) {
            p->finish_time = current_time + 1; // finishes at end of this cycle
            pthread_mutex_unlock(&p->lock);
            break;
        }
        pthread_mutex_unlock(&p->lock);
        // block again: next sem_wait occurs on next scheduler dispatch
    }
    // thread ends
    return NULL;
}

// parse CSV line
int parse_csv_line(char *line, char **pid_out, int *arrival_out, int *burst_out, int *prio_out) {
    // trim newline
    char *p = line;
    char *fields[4] = {0};
    int f = 0;
    char *token;
    // split by comma
    token = strtok(p, ",");
    while (token && f < 4) {
        while (*token && isspace((unsigned char)*token)) ++token;
        fields[f++] = token;
        token = strtok(NULL, ",");
    }
    if (f < 4) return -1;
    *pid_out = strdup(fields[0]);
    *arrival_out = atoi(fields[1]);
    *burst_out = atoi(fields[2]);
    *prio_out = atoi(fields[3]);
    return 0;
}

// add process to processes array
void add_process(const char *pid, int arrival, int burst, int priority) {
    if (proc_count >= capacity) {
        capacity = capacity == 0 ? 8 : capacity * 2;
        processes = realloc(processes, sizeof(process_t)*capacity);
    }
    int i = proc_count++;
    processes[i].pid = strdup(pid);
    processes[i].arrival = arrival;
    processes[i].burst = burst;
    processes[i].remaining = burst;
    processes[i].priority = priority;
    processes[i].started = 0;
    processes[i].start_time = -1;
    processes[i].finish_time = -1;
    processes[i].admitted = 0;
    pthread_mutex_init(&processes[i].lock, NULL);
    // Create a unique named semaphore for this process using parent PID and index, retrying if it already exists
    snprintf(processes[i].sem_name, sizeof(processes[i].sem_name), "/schedsim_%d_%d", (int)getpid(), i);
    processes[i].sem = sem_open(processes[i].sem_name, O_CREAT | O_EXCL, 0600, 0);
    if (processes[i].sem == SEM_FAILED) {
        if (errno == EEXIST) {
            /* Try unlinking leftovers and retry once */
            sem_unlink(processes[i].sem_name);
            processes[i].sem = sem_open(processes[i].sem_name, O_CREAT | O_EXCL, 0600, 0);
        }
    }
    if (processes[i].sem == SEM_FAILED) die("sem_open");
}

int all_finished() {
    return total_finished >= proc_count;
}

void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "Options:\n"
        "  -f, --fcfs            Use FCFS scheduling\n"
        "  -s, --sjf             Use SJF scheduling (non-preemptive)\n"
        "  -r, --rr              Use Round Robin scheduling\n"
        "  -p, --priority        Use Priority scheduling (preemptive, lower value = higher priority)\n"
        "  -i, --input <file>    Input CSV filename (pid,arrival,burst,priority)\n"
        "  -q, --quantum <n>     Time quantum for RR (required for -r)\n"
        , prog);
}

// main scheduler loop
void scheduler_loop() {
    // spawn threads now (each thread will block on its sem)
    for (int i = 0; i < proc_count; ++i) {
        if (pthread_create(&processes[i].thread, NULL, process_thread, (void*)(intptr_t)i) != 0) die("pthread_create");
    }

    current_time = 0;
    total_finished = 0;
    cpu_busy_cycles = 0;

    // For RR: maintain an index into ready queue for next selection
    int rr_index = 0;
    int rr_slice_used = 0;
    int running_proc = -1; // index of currently running process (if scheduler wants to persist)
    int running_start_time = -1;

    // For SJF non-preemptive we will pick a process and keep it until finished
    int sjf_locked_proc = -1;

    // Simulation loop: advance until all processes finished
    while (!all_finished()) {
        // 1) Admit arrivals whose arrival <= current_time
        for (int i = 0; i < proc_count; ++i) {
            if (processes[i].arrival <= current_time && processes[i].start_time == -1 && processes[i].remaining == processes[i].burst && !processes[i].admitted) {
                // arrived but not yet added
                // Add to READY queue according to algorithm
                if (algorithm == ALG_FCFS || algorithm == ALG_RR) {
                    ready_push_back(i);
                    processes[i].admitted = 1;
                } else if (algorithm == ALG_SJF || algorithm == ALG_PRIORITY) {
                    // insert ordered
                    ready_insert_ordered(i);
                    processes[i].admitted = 1;
                }
            }
        }

        // If READY queue empty, CPU idle for this cycle, unless an algorithm has a currently-running process that should continue (FCFS/SJF/RR).
        if (ready_count == 0) {
            int has_running = 0;
            if (algorithm == ALG_FCFS && running_proc != -1) has_running = 1;
            if (algorithm == ALG_SJF && sjf_locked_proc != -1) has_running = 1;
            if (algorithm == ALG_RR && running_proc != -1) has_running = 1;
            if (!has_running) {
                if (current_time > 1000) {
                    /* Long idle — dump diagnostic once then exit to avoid spinning forever. */
                    printf("[DIAG] ready empty at t=%d, total_finished=%d, proc_count=%d\n", current_time, total_finished, proc_count);
                    for (int k = 0; k < proc_count; ++k) {
                        printf("[DIAG] P=%s arr=%d rem=%d start=%d fin=%d admitted=%d started=%d\n",
                            processes[k].pid, processes[k].arrival, processes[k].remaining, processes[k].start_time, processes[k].finish_time, processes[k].admitted, processes[k].started);
                    }
                    fflush(stdout);
                    exit(2);
                }
                // No one is runnable and no running process -> idle
                current_time++;
                continue;
            }
        }

        // 2) Select next process based on algorithm
        int pick = -1;
        if (algorithm == ALG_FCFS) {
            // FCFS: pick front, and run it until completion (i.e., keep dispatching it every cycle until done)
            if (running_proc == -1) {
                // pick new front
                pick = ready_pop_front();
                running_proc = pick;
                running_start_time = current_time;
            } else {
                pick = running_proc;
            }
        } else if (algorithm == ALG_SJF) {
            // SJF non-preemptive: choose smallest remaining, then keep until done
            if (sjf_locked_proc == -1) {
                // pick front (ready_insert_ordered kept it sorted by remaining)
                pick = ready_pop_front();
                sjf_locked_proc = pick;
                running_start_time = current_time;
            } else {
                pick = sjf_locked_proc;
            }
        } else if (algorithm == ALG_RR) {
            // RR: rotate through ready queue, each process gets up to quantum cycles
            if (running_proc == -1) {
                // get first ready
                pick = ready_pop_front();
                running_proc = pick;
                rr_slice_used = 0;
                running_start_time = current_time;
            } else {
                pick = running_proc;
            }
        } else if (algorithm == ALG_PRIORITY) {
            // Preemptive priority: pick highest priority (ready_insert_ordered keeps ready sorted by priority ascending)
            pick = ready_pop_front();
        }

        // skip this cycle if the selected process index is invalid

        if (pick < 0 || pick >= proc_count) { current_time++; continue; }

        // 3) Dispatch the process: post its semaphore so it executes exactly one cycle
        // Record that CPU is busy this cycle
        cpu_busy_cycles++;

        // For Gantt: if switching process, close prior segment and start new
        const char *pidstr = processes[pick].pid;
        // If last segment has same pid and one line, gantt_append will merge
        gantt_append(current_time, current_time+1, pidstr);

        // post semaphore
        sem_post(processes[pick].sem);

        // Advance time by one cycle (the thread uses current_time to set start/finish; threads see global current_time)
        current_time++;

        // After this cycle, check if the process finished
        pthread_mutex_lock(&processes[pick].lock);
        int finished_now = (processes[pick].remaining == 0);
        pthread_mutex_unlock(&processes[pick].lock);

        if (finished_now) {
            total_finished++;
            // ensure it is not in ready queue (it shouldn't be)
            int pos = ready_find_pos(pick);
            if (pos != -1) ready_remove_pos(pos);

            // release locks for algorithms that held a running proc
            if (algorithm == ALG_FCFS && running_proc == pick) running_proc = -1;
            if (algorithm == ALG_SJF && sjf_locked_proc == pick) sjf_locked_proc = -1;
            if (algorithm == ALG_RR && running_proc == pick) running_proc = -1;
            // For priority
            continue;
        } else {
            // not finished: for non-preemptive algorithms may requeue or keep running
            if (algorithm == ALG_FCFS) {
                // keep running_proc, requeue not needed
            } else if (algorithm == ALG_SJF) {
                // keep sjf_locked_proc same
            } else if (algorithm == ALG_RR) {
                rr_slice_used++;
                if (rr_slice_used >= quantum) {
                    // quantum exhausted -> requeue at end
                    ready_push_back(pick);
                    running_proc = -1;
                    rr_slice_used = 0;
                } else {
                    // continue running same process next cycle: do nothing
                }
            } else if (algorithm == ALG_PRIORITY) {
                // after one cycle, reinsert proc into ready queue (it's still runnable)
                // must maintain ordering by priority: insert ordered
                ready_insert_ordered(pick);
            }
        }
    }

    // All finished: join threads
    for (int i = 0; i < proc_count; ++i) {
        pthread_join(processes[i].thread, NULL);
    }
}

// print results
void print_report() {
    printf("===== Schedule Results =====\n\n");

    // Print Gantt chart numeric timeline and pid bars
    printf("Timeline (Gantt Chart):\n");
    // print time ticks top
    for (int i = 0; i <= gantt_count; ++i) {
        // print timeline as segments with start/end and pid below
    }
    // Print segments with times
    for (int i = 0; i < gantt_count; ++i) {
        printf("%d-%d: %s  ", gantt[i].start, gantt[i].end, gantt[i].pid);
    }
    printf("\n\n");

    // Table header
    printf("PID\tArr\tBurst\tStart\tFinish\tWait\tResp\tTurn\n");
    double total_wait = 0.0, total_resp = 0.0, total_turn = 0.0;
    for (int i = 0; i < proc_count; ++i) {
        int arr = processes[i].arrival;
        int burst = processes[i].burst;
        int start = processes[i].start_time;
        int finish = processes[i].finish_time;
        int turnaround = finish - arr;
        int wait = turnaround - burst;
        int resp = start - arr;
        if (start < 0) resp = 0; // safety
        printf("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
            processes[i].pid, arr, burst, start, finish, wait, resp, turnaround);
        total_wait += wait;
        total_resp += resp;
        total_turn += turnaround;
    }
    double n = proc_count;
    printf("\nAvg Wait = %.2f\nAvg Resp = %.2f\nAvg Turn = %.2f\n",
        total_wait/n, total_resp/n, total_turn/n);
    double throughput = n / (double)current_time;
    double cpu_util = 100.0 * ((double)cpu_busy_cycles / (double)current_time);
    printf("Throughput = %.4f jobs/unit time\n", throughput);
    printf("CPU Utilization = %.2f%%\n", cpu_util);
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
        {"fcfs", no_argument, 0, 'f'},
        {"sjf", no_argument, 0, 's'},
        {"rr", no_argument, 0, 'r'},
        {"priority", no_argument, 0, 'p'},
        {"input", required_argument, 0, 'i'},
        {"quantum", required_argument, 0, 'q'},
        {0,0,0,0}
    };

    char *input_file = NULL;
    algorithm = ALG_FCFS;

    int opt;
    int opt_index = 0;
    while ((opt = getopt_long(argc, argv, "fsrpi:q:", long_options, &opt_index)) != -1) {
        switch (opt) {
            case 'f': algorithm = ALG_FCFS; break;
            case 's': algorithm = ALG_SJF; break;
            case 'r': algorithm = ALG_RR; break;
            case 'p': algorithm = ALG_PRIORITY; break;
            case 'i': input_file = strdup(optarg); break;
            case 'q':
                quantum = atoi(optarg);
                if (quantum <= 0) { fprintf(stderr, "Invalid quantum\n"); return 1; }
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!input_file) {
        fprintf(stderr, "Input file is required (-i)\n");
        print_usage(argv[0]);
        return 1;
    }
    if (algorithm == ALG_RR && quantum <= 0) {
        fprintf(stderr, "Round Robin requires a positive quantum (-q)\n");
        return 1;
    }

    // parse CSV input file
    FILE *f = fopen(input_file, "r");
    if (!f) { perror("fopen"); return 1; }

    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    int line_num = 0;

    while ((read = getline(&line, &len, f)) != -1) {
        line_num++;
        if (line_num == 1) continue; // skip header line

        // skip empty or comment lines
        char *s = line;
        while (*s && isspace((unsigned char)*s)) ++s;
        if (*s == '\0' || *s == '#') continue;

        // strip newline
        char *nl = strchr(s, '\n');
        if (nl) *nl = '\0';

        char *pid; int arr, burst, prio;
        char *tmp = strdup(s);
        if (parse_csv_line(tmp, &pid, &arr, &burst, &prio) == 0) {
            add_process(pid, arr, burst, prio);
            free(pid);
        }
        free(tmp);
    }

    free(line);
    fclose(f);


    if (proc_count == 0) {
        fprintf(stderr, "No processes loaded.\n");
        return 1;
    }
    // Sort processes by arrival time for deterministic admission order (not strictly required)
    // We'll keep initial array as-is; admission checks look at arrival times.

    // Run scheduler loop
    scheduler_loop();

    // Print report
    print_report();

    // cleanup: close and unlink named semaphores
    for (int i = 0; i < proc_count; ++i) {
        if (processes[i].sem && processes[i].sem != SEM_FAILED) {
            sem_close(processes[i].sem);
            /* Unlink the named semaphore so it is removed from the system. */
            sem_unlink(processes[i].sem_name);
        }
        pthread_mutex_destroy(&processes[i].lock);
        free(processes[i].pid);
    }
    for (int i = 0; i < gantt_count; ++i) free(gantt[i].pid);
    free(gantt);
    free(processes);
    free(ready);
    free(input_file);
    return 0;
}