/*
setting c: aps partition experiment, local, with inheritance.
this experiment validates r9-a from the paper

r9-a says: when a client sends a message to a server on the same node,
the server's cpu time for handling that request gets billed to the
client's partition, not the server's own partition. this is called
partition inheritance.

what this does:
  - creates two aps partitions: "clients" (50%) and "server_part" (30%)
  - system partition keeps the remaining 20%
  - puts all 4 client threads in the clients partition
  - puts the server thread in the server_part partition
  - channel created with inheritance enabled
  - after each MsgReceive, queries which partition the server is
    currently billing to using SCHED_APS_QUERY_THREAD
  - logs the server's home partition vs inherited partition at each step
  - prints partition cpu usage stats at the end of the run


thread parameters (same as setting a, shifted down by 14):
  c1 = (200, 10,  0, 236)
  c2 = (200, 10,  6, 239)
  c3 = (200, 10, 12, 241)
  c4 = (200, 10, 18, 240)
  s1 = priority 238

REQUIRES QNX 7.1 BUILDFILE needs :[module=aps] on the procnto line
must run as root
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/sched_aps.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#define RUN_DURATION_S  5
#define NUM_CLIENTS     4
#define NS_TO_MS(ns)    ((double)(ns) / 1000000.0)
#define SERVER_PRIORITY 238

// partition budgets as percentages
#define CLIENT_PARTITION_BUDGET  50
#define SERVER_PARTITION_BUDGET  30

static const int client_params[NUM_CLIENTS][4] = {
    {200, 10,  0, 236},
    {200, 10,  6, 239},
    {200, 10, 12, 241},
    {200, 10, 18, 240},
};

static volatile int g_chid = -1;
static volatile int g_coid = -1;
static struct timespec g_start;
static volatile int g_running = 1;

// partition ids filled in after creation
static int g_client_part_id = -1;
static int g_server_part_id = -1;

#define LOG_MAX 12288

typedef struct {
    double time_ms;
    int    thread_id;
    char   event[48];
    int    priority;
    int    home_part;
    int    inherited_part;
} log_entry_t;

static log_entry_t     g_log[LOG_MAX];
static volatile int    g_log_idx = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint16_t type;
    uint16_t subtype;
    int      client_id;
} client_msg_t;

typedef struct {
    int ack;
} server_reply_t;

typedef struct {
    int id;
    int period_ms;
    int wcet_ms;
    int offset_ms;
    int priority;
} client_arg_t;

static double elapsed_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long ns = (long long)(now.tv_sec  - g_start.tv_sec)  * 1000000000LL
                 + (long long)(now.tv_nsec - g_start.tv_nsec);
    return NS_TO_MS(ns);
}

static void sleep_ms(long long ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000LL;
    nanosleep(&ts, NULL);
}

static void burn_cpu_ms(long long ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    long long target_ns = ms * 1000000LL;
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (((long long)(now.tv_sec  - start.tv_sec)  * 1000000000LL
            + (long long)(now.tv_nsec - start.tv_nsec)) < target_ns);
}

/*
input: thread_id, event string, priority, home partition, inherited partition
output: none
appends one entry to the global log array with timestamp, priority,
and partition info. uses a mutex so threads don't overwrite each other.
*/
static void log_event(int thread_id, const char *event, int priority,
                      int home_part, int inherited_part) {
    pthread_mutex_lock(&g_log_mutex);
    int idx = g_log_idx;
    if (idx < LOG_MAX) {
        g_log[idx].time_ms        = elapsed_ms();
        g_log[idx].thread_id      = thread_id;
        strncpy(g_log[idx].event, event, sizeof(g_log[idx].event) - 1);
        g_log[idx].event[sizeof(g_log[idx].event) - 1] = '\0';
        g_log[idx].priority       = priority;
        g_log[idx].home_part      = home_part;
        g_log[idx].inherited_part = inherited_part;
        g_log_idx++;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

/*
input: none
output: none
prints the full execution trace sorted by time.
*/
static void print_log(void) {
    int n = g_log_idx;
    // simple insertion sort by time
    for (int i = 1; i < n; i++) {
        log_entry_t tmp = g_log[i];
        int j = i - 1;
        while (j >= 0 && g_log[j].time_ms > tmp.time_ms) {
            g_log[j + 1] = g_log[j];
            j--;
        }
        g_log[j + 1] = tmp;
    }

    printf("\n========== execution trace (aps partition inheritance) ==========\n");
    printf("%-12s %-8s %-28s %-10s %-6s %-6s\n",
           "time(ms)", "thread", "event", "priority", "home", "inher");
    printf("%-12s %-8s %-28s %-10s %-6s %-6s\n",
           "--------", "------", "-----", "--------", "----", "-----");
    for (int i = 0; i < n; i++) {
        const char *name;
        if (g_log[i].thread_id == 0)
            name = "s1";
        else {
            static char buf[8];
            snprintf(buf, sizeof(buf), "c%d", g_log[i].thread_id);
            name = buf;
        }
        printf("%10.4f  %-8s %-28s %-10d %-6d %-6d\n",
               g_log[i].time_ms, name, g_log[i].event,
               g_log[i].priority, g_log[i].home_part,
               g_log[i].inherited_part);
    }
    printf("=================================================================\n");
}

/*
input: none
output: the calling thread's current priority
*/
static int current_thread_priority(void) {
    struct sched_param sp;
    int policy;
    pthread_getschedparam(pthread_self(), &policy, &sp);
    return sp.sched_priority;
}

/*
input: pointers to home and inherited partition id variables
output: fills in home and inherited via SchedCtl query
queries the aps scheduler for the calling thread's partition info.
*/
static void query_thread_partition(int *home, int *inherited) {
    sched_aps_query_thread_parms qt;
    APS_INIT_DATA(&qt);
    qt.pid = 0;
    qt.tid = 0;
    if (SchedCtl(SCHED_APS_QUERY_THREAD, &qt, sizeof(qt)) == EOK) {
        *home = qt.id;
        *inherited = qt.inherited_id;
    } else {
        *home = -1;
        *inherited = -1;
    }
}

/*
input: event string
output: none
queries the server's current partition assignment so we can see
partition inheritance happening in the trace.
*/
static void log_server_event(const char *event) {
    int home, inherited;
    query_thread_partition(&home, &inherited);
    log_event(0, event, current_thread_priority(), home, inherited);
}

/*
input: partition name string, budget percentage
output: returns the partition id on success, -1 on failure
creates an aps partition with the given name and cpu budget.
*/
static int create_partition(const char *name, int budget_percent) {
    sched_aps_create_parms cp;
    APS_INIT_DATA(&cp);
    cp.name = (char *)name;
    cp.budget_percent = budget_percent;
    cp.critical_budget_ms = 0;

    int ret = SchedCtl(SCHED_APS_CREATE_PARTITION, &cp, sizeof(cp));
    if (ret != EOK) {
        fprintf(stderr, "failed to create partition \"%s\" (%d%%): %s (%d)\n",
                name, budget_percent, strerror(errno), errno);
        return -1;
    }
    printf("[aps] created partition \"%s\" (id=%d, budget=%d%%)\n",
           name, cp.id, budget_percent);
    return cp.id;
}

/*
input: partition id to join
output: returns 0 on success, -1 on failure
joins the calling thread to the specified partition.
*/
static int join_partition(int part_id) {
    sched_aps_join_parms jp;
    APS_INIT_DATA(&jp);
    jp.id = part_id;
    jp.pid = 0;
    jp.tid = 0;

    int ret = SchedCtl(SCHED_APS_JOIN_PARTITION, &jp, sizeof(jp));
    if (ret != EOK) {
        fprintf(stderr, "failed to join partition %d: %s (%d)\n",
                part_id, strerror(errno), errno);
        return -1;
    }
    return 0;
}

/*
input: none
output: none
queries and prints cpu usage stats for all partitions.
*/
static void print_partition_stats(void) {
    sched_aps_info info;
    APS_INIT_DATA(&info);
    if (SchedCtl(SCHED_APS_QUERY_PARMS, &info, sizeof(info)) != EOK) {
        perror("SCHED_APS_QUERY_PARMS");
        return;
    }

    printf("\n========== partition cpu stats ==========\n");

    uint64_t total_cycles = 0;

    // first pass: get total cycles across all partitions
    for (int i = 0; i < info.num_partitions; i++) {
        sched_aps_partition_stats ps;
        APS_INIT_DATA(&ps);
        ps.id = i;
        if (SchedCtl(SCHED_APS_PARTITION_STATS, &ps, sizeof(ps)) == EOK) {
            total_cycles += ps.run_time_cycles;
        }
    }

    // second pass: print each partition's share
    for (int i = 0; i < info.num_partitions; i++) {
        sched_aps_partition_stats ps;
        APS_INIT_DATA(&ps);
        ps.id = i;
        if (SchedCtl(SCHED_APS_PARTITION_STATS, &ps, sizeof(ps)) == EOK) {
            double pct = 0.0;
            if (total_cycles > 0)
                pct = (double)ps.run_time_cycles / (double)total_cycles * 100.0;
            printf("partition %d (%s): budget=%d%%  actual_cpu=%.1f%%\n",
                   i, ps.name, ps.budget_percent, pct);
        }
    }
    printf("=========================================\n");
}

/*
input: arg, unused
output: NULL
the server thread. joins the server_part partition, creates a channel
with inheritance enabled, then loops on MsgReceive. after each receive,
queries the partition info to detect partition inheritance.
*/
static void *server_thread(void *arg) {
    (void)arg;

    // join the server partition
    if (join_partition(g_server_part_id) == -1) {
        fprintf(stderr, "[s1] failed to join server_part\n");
        return NULL;
    }
    printf("[s1] joined server_part (partition %d), priority %d\n",
           g_server_part_id, SERVER_PRIORITY);

    log_server_event("STARTUP");

    // create channel with inheritance enabled
    g_chid = ChannelCreate(0);
    if (g_chid == -1) {
        perror("ChannelCreate");
        return NULL;
    }

    log_server_event("RECEIVE_BLOCKED");

    client_msg_t   msg;
    server_reply_t reply;
    reply.ack = 1;
    struct _msg_info info;

    while (g_running) {
        int rcvid = MsgReceive(g_chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) {
            if (!g_running) break;
            perror("MsgReceive");
            break;
        }

        log_server_event("RUNNING_AFTER_RECEIVE");

        printf("[s1] handling request from c%d\n", msg.client_id);

        log_server_event("SERVICE_BEGIN");
        burn_cpu_ms(10);
        log_server_event("SERVICE_END");

        MsgReply(rcvid, EOK, &reply, sizeof(reply));

        log_server_event("AFTER_REPLY");

        // check if more messages are waiting
        // if not, we'll go back to RECEIVE_BLOCKED
    }

    ChannelDestroy(g_chid);
    printf("[s1] done\n");
    return NULL;
}

/*
input: arg, pointer to client_arg_t with id, period, wcet, offset, priority
output: NULL
client thread. joins the clients partition, then loops: burn cpu for
half the wcet, send a message, burn cpu for the other half, sleep.
*/
static void *client_thread(void *arg) {
    client_arg_t *p = (client_arg_t *)arg;

    // wait for offset before starting
    if (p->offset_ms > 0)
        sleep_ms(p->offset_ms);

    // join the clients partition
    if (join_partition(g_client_part_id) == -1) {
        fprintf(stderr, "[c%d] failed to join clients partition\n", p->id);
        return NULL;
    }
    printf("[c%d] joined clients (partition %d), priority %d\n",
           p->id, g_client_part_id, p->priority);

    client_msg_t   msg;
    server_reply_t reply;
    msg.type    = 0x0100;
    msg.subtype = 0;
    msg.client_id = p->id;

    while (g_running) {
        log_event(p->id, "READY", p->priority,
                  g_client_part_id, g_client_part_id);
        burn_cpu_ms(p->wcet_ms / 2);

        log_event(p->id, "SEND_BLOCKED", p->priority,
                  g_client_part_id, g_client_part_id);

        int ret = MsgSend(g_coid, &msg, sizeof(msg), &reply, sizeof(reply));
        if (ret == -1) {
            if (!g_running) break;
            perror("MsgSend error");
            break;
        }

        log_event(p->id, "RUNNING", p->priority,
                  g_client_part_id, g_client_part_id);

        burn_cpu_ms(p->wcet_ms / 2);

        long long remaining = (long long)p->period_ms - (long long)p->wcet_ms;
        if (remaining > 0) sleep_ms(remaining);
    }

    printf("[c%d] done\n", p->id);
    return NULL;
}

int main(void) {
    printf("=== setting c: aps partition experiment (r9-a) ===\n");
    printf("target: validate partition inheritance by observing\n");
    printf("        which partition the server bills to during message handling\n\n");

    clock_gettime(CLOCK_MONOTONIC, &g_start);

    // check if aps is running
    sched_aps_info aps_check;
    APS_INIT_DATA(&aps_check);
    if (SchedCtl(SCHED_APS_QUERY_PARMS, &aps_check, sizeof(aps_check)) != EOK) {
        fprintf(stderr,
            "error: aps scheduler is not running.\n"
            "make sure your qnx image has [module=aps] on the procnto line.\n"
            "you can check with: aps show\n");
        return EXIT_FAILURE;
    }
    printf("[aps] scheduler is running. %d partition(s) currently defined.\n\n",
           aps_check.num_partitions);

    // create partitions
    g_client_part_id = create_partition("clients", CLIENT_PARTITION_BUDGET);
    if (g_client_part_id == -1) return EXIT_FAILURE;

    g_server_part_id = create_partition("server_part", SERVER_PARTITION_BUDGET);
    if (g_server_part_id == -1) return EXIT_FAILURE;

    printf("[aps] system partition keeps %d%%\n\n",
           100 - CLIENT_PARTITION_BUDGET - SERVER_PARTITION_BUDGET);

    // start server thread
    pthread_t          server_tid;
    pthread_attr_t     server_attr;
    struct sched_param server_sp;
    int                ret;

    pthread_attr_init(&server_attr);
    pthread_attr_setinheritsched(&server_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&server_attr, SCHED_FIFO);
    server_sp.sched_priority = SERVER_PRIORITY;
    pthread_attr_setschedparam(&server_attr, &server_sp);

    ret = pthread_create(&server_tid, &server_attr, server_thread, NULL);
    if (ret != 0) {
        fprintf(stderr, "failed to create server thread: %s\n", strerror(ret));
        return EXIT_FAILURE;
    }
    pthread_attr_destroy(&server_attr);

    // wait for server channel to be ready
    while (g_chid == -1) usleep(1000);

    g_coid = ConnectAttach(0, 0, g_chid, _NTO_SIDE_CHANNEL, 0);
    if (g_coid == -1) {
        perror("ConnectAttach failed");
        return EXIT_FAILURE;
    }
    printf("[main] connected to server channel (chid=%d, coid=%d)\n\n",
           g_chid, g_coid);

    // start client threads
    pthread_t    client_tids[NUM_CLIENTS];
    client_arg_t client_args[NUM_CLIENTS];

    for (int i = 0; i < NUM_CLIENTS; i++) {
        client_args[i].id        = i + 1;
        client_args[i].period_ms = client_params[i][0];
        client_args[i].wcet_ms   = client_params[i][1];
        client_args[i].offset_ms = client_params[i][2];
        client_args[i].priority  = client_params[i][3];

        pthread_attr_t     attr;
        struct sched_param sp;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        sp.sched_priority = client_params[i][3];
        pthread_attr_setschedparam(&attr, &sp);

        ret = pthread_create(&client_tids[i], &attr, client_thread,
                             &client_args[i]);
        if (ret != 0) {
            fprintf(stderr, "failed to create c%d (priority %d): %s\n",
                    i + 1, client_params[i][3], strerror(ret));
        } else {
            printf("[main] created c%d at priority %d\n",
                   i + 1, client_params[i][3]);
        }
        pthread_attr_destroy(&attr);
    }

    printf("\n[main] aps experiment running for %d seconds...\n\n", RUN_DURATION_S);

    sleep(RUN_DURATION_S);
    g_running = 0;
    sleep(1);

    // print the execution trace
    print_log();

    // print partition cpu stats
    print_partition_stats();

    ConnectDetach(g_coid);

    printf("\nkey things to check for r9-a:\n");
    printf("  1) in the trace, look at the 'home' and 'inher' columns for s1\n");
    printf("     home should be %d (server_part)\n", g_server_part_id);
    printf("     inher should be %d (clients) during RUNNING_AFTER_RECEIVE / SERVICE_BEGIN\n",
           g_client_part_id);
    printf("  2) in the partition stats, the clients partition cpu usage should be\n");
    printf("     higher than expected because server work is billed there\n");
    printf("  3) the server_part cpu usage should be lower than its 30%% budget\n");

    return EXIT_SUCCESS;
}
