/*
setting a: baseline ipc experiment, no inheritance, no aps partitions.

this is the first experiment we need to run to replicate the paper

what setting a does:
  - runs 4 client threads and 1 server thread on the same machine
  - no aps budget partitions, no priority inheritance
  - each client does cpu work for half its wcet, then sends a message
    to the server and waits for a reply
  - the server receives messages and handles them one at a time
  - we log every state change with a timestamp so we can compare
    against the paper's figure 2a, listing 1, and listing 2

thread parameters from the paper (period ms, wcet ms, offset ms, priority):
  c1 = (200, 10,  0, 236)   paper value: 250
  c2 = (200, 10,  6, 239)   paper value: 253
  c3 = (200, 10, 12, 241)   paper value: 255
  c4 = (200, 10, 18, 240)   paper value: 254
  s1 = priority 238         paper value: 252

note on priorities: qnx reserves the highest priority levels for
privileged processes. since we may not be running as root, we shift
all priorities down by 14 so they stay in the safe unprivileged range.
the relative ordering between threads is preserved exactly, which is
what matters for validating the scheduling rules.

rules we expect to confirm from the paper:
  r1: client starts in ready state at its release time
  r2: client calls msgsend and becomes send_blocked
  r3: server wakes up when a message arrives
  r4: server goes receive_blocked when no messages are waiting
  r5: client goes reply_blocked while server works, then running
  r6: server finishes one request fully before taking the next
  r7: server handles messages in priority order, highest first

build this as a c qnx application in momentics (x86 target).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>

// how long the whole experiment runs in seconds
#define RUN_DURATION_S  5

// number of client threads
#define NUM_CLIENTS     4

// converts nanoseconds to milliseconds as a double
#define NS_TO_MS(ns)    ((double)(ns) / 1000000.0)

// server priority, shifted down 14 from the paper's 252
#define SERVER_PRIORITY 238

// client parameters: {period ms, wcet ms, offset ms, priority}
// priorities are shifted down 14 from the paper but ordering is the same
static const int client_params[NUM_CLIENTS][4] = {
    {200, 10,  0, 236},   // c1 (paper: 250)
    {200, 10,  6, 239},   // c2 (paper: 253)
    {200, 10, 12, 241},   // c3 (paper: 255)
    {200, 10, 18, 240},   // c4 (paper: 254)
};

// channel id the server creates, set once ChannelCreate succeeds
// starts at -1 so clients know to wait
static volatile int g_chid = -1;

// connection id that clients use to send messages to the server
// starts at -1 so clients know to wait until main connects
static volatile int g_coid = -1;

// experiment start time, used to compute elapsed ms for all log entries
static struct timespec g_start;

// set to 0 when the experiment is done to tell threads to stop
static volatile int g_running = 1;


// ---------------------------------------------------------------
// log structure
// ---------------------------------------------------------------

// maximum number of log entries across the whole experiment
#define LOG_MAX 8192

// one entry per state transition event
typedef struct {
    double time_ms;      // ms since experiment started
    int    thread_id;    // 0 = server, 1..4 = clients
    char   event[32];    // state name e.g. "READY", "SEND_BLOCKED"
    int    priority;     // thread priority at this moment
} log_entry_t;

static log_entry_t     g_log[LOG_MAX];
static volatile int    g_log_idx = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;


// ---------------------------------------------------------------
// message types
// ---------------------------------------------------------------

// the message a client sends to the server
// qnx requires the first two bytes to be a type field
typedef struct {
    uint16_t type;
    uint16_t subtype;
    int      client_id;   // which client is sending this
} client_msg_t;

// the reply the server sends back to the client
typedef struct {
    int ack;
} server_reply_t;


/*
elapsed_ms

input: nothing
output: double, milliseconds since record_start was called

used to timestamp every log entry. all times are relative to
the moment the experiment started so they are easy to compare.
*/
static double elapsed_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long ns = (long long)(now.tv_sec  - g_start.tv_sec)  * 1000000000LL
                 + (long long)(now.tv_nsec - g_start.tv_nsec);
    return NS_TO_MS(ns);
}


/*
sleep_ms

input: ms, number of milliseconds to sleep
output: nothing

sleeps the calling thread for the given duration.
used for period waits and initial offsets.
*/
static void sleep_ms(long long ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000LL;
    nanosleep(&ts, NULL);
}


/*
burn_cpu_ms

input: ms, how many milliseconds of cpu work to simulate
output: nothing

busy-waits for the given duration to simulate cpu computation.
we use this instead of sleep because sleep yields the cpu,
which is not the same as a thread actually doing work.
this is how we model the wcet of each thread.
*/
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
log_event

input: thread_id (0=server, 1-4=clients), event name string, priority
output: nothing

appends one state transition entry to the global log array.
uses a mutex so multiple threads dont overwrite each other.
silently drops entries if the log is full.
*/
static void log_event(int thread_id, const char *event, int priority) {
    pthread_mutex_lock(&g_log_mutex);
    int idx = g_log_idx;
    if (idx < LOG_MAX) {
        g_log[idx].time_ms   = elapsed_ms();
        g_log[idx].thread_id = thread_id;
        strncpy(g_log[idx].event, event, sizeof(g_log[idx].event) - 1);
        g_log[idx].event[sizeof(g_log[idx].event) - 1] = '\0';
        g_log[idx].priority  = priority;
        g_log_idx++;
    }
    pthread_mutex_unlock(&g_log_mutex);
}


/*
print_log

input: nothing
output: nothing

sorts all log entries by timestamp and prints them as a table.
this is the execution trace we compare against the paper's
figure 2a, listing 1, and listing 2.

uses insertion sort since the log is small enough that a
fancier sort is not necessary.
*/
static void print_log(void) {
    printf("\n========== execution trace ==========\n");
    printf("%-14s %-8s %-20s %-8s\n", "time(ms)", "thread", "event", "priority");
    printf("%-14s %-8s %-20s %-8s\n", "--------", "------", "-----", "--------");

    // sort by time_ms so the trace reads chronologically
    for (int i = 1; i < g_log_idx; i++) {
        log_entry_t key = g_log[i];
        int j = i - 1;
        while (j >= 0 && g_log[j].time_ms > key.time_ms) {
            g_log[j + 1] = g_log[j];
            j--;
        }
        g_log[j + 1] = key;
    }

    // map thread_id to a readable name for the table
    for (int i = 0; i < g_log_idx; i++) {
        const char *name;
        switch (g_log[i].thread_id) {
            case 0:  name = "s1"; break;
            case 1:  name = "c1"; break;
            case 2:  name = "c2"; break;
            case 3:  name = "c3"; break;
            case 4:  name = "c4"; break;
            default: name = "??"; break;
        }
        printf("%-14.4f %-8s %-20s %-8d\n",
               g_log[i].time_ms, name, g_log[i].event, g_log[i].priority);
    }
    printf("=====================================\n");
}


/*
server_thread

input: arg, unused (required by pthread signature)
output: NULL

this is the server. it creates a channel, then loops waiting for
messages from clients. when a message arrives it logs the state
transitions, simulates 10ms of service work, then replies.

also logs reply_blocked on behalf of the client that sent the
current message. the client is stuck inside msgsend and cannot
log for itself, so we do it here right after picking up the message.
client_id is 1-based so we subtract 1 to index into client_params.

this validates r3, r4, r5, r6, and r7 from the paper.
*/
static void *server_thread(void *arg) {
    (void)arg;

    // create the channel that clients will connect to
    g_chid = ChannelCreate(0);
    if (g_chid == -1) {
        perror("ChannelCreate failed");
        exit(EXIT_FAILURE);
    }

    printf("[s1] started at priority %d, channel id = %d\n",
           SERVER_PRIORITY, g_chid);

    client_msg_t   msg;
    server_reply_t reply;
    int            rcvid;

    while (g_running) {

        // r4: server blocks here waiting for a message
        log_event(0, "RECEIVE_BLOCKED", SERVER_PRIORITY);

        rcvid = MsgReceive(g_chid, &msg, sizeof(msg), NULL);

        // -1 means error or channel was destroyed on shutdown
        if (rcvid == -1) {
            if (!g_running) break;
            perror("MsgReceive error");
            break;
        }

        // r3: message arrived, server is now running
        log_event(0, "RUNNING", SERVER_PRIORITY);

        // r5: the client that sent this is now reply_blocked.
        // log it here since the client cant log while stuck in msgsend.
        log_event(msg.client_id, "REPLY_BLOCKED",
                  client_params[msg.client_id - 1][3]);

        printf("[s1] %.4f ms  handling request from c%d\n",
               elapsed_ms(), msg.client_id);

        // r6: run this request to completion before taking another
        burn_cpu_ms(10);

        // reply moves the client from reply_blocked back to ready
        reply.ack = 1;
        MsgReply(rcvid, EOK, &reply, sizeof(reply));

        // server briefly goes ready before looping back to receive_blocked
        log_event(0, "READY", SERVER_PRIORITY);
    }

    printf("[s1] shutting down\n");
    ChannelDestroy(g_chid);
    return NULL;
}


/*
client_arg_t

holds the parameters for one client thread.
passed as the argument to client_thread.
*/
typedef struct {
    int id;           // client number 1..4
    int period_ms;    // how often the client wakes up in ms
    int wcet_ms;      // worst case execution time in ms
    int offset_ms;    // initial delay before first period starts
    int priority;     // thread priority
} client_arg_t;


/*
client_thread

input: arg, pointer to a client_arg_t with this client's parameters
output: NULL

waits for its initial offset to stagger the start, then each period:
  1. logs ready (r1)
  2. burns cpu for half its wcet (work before the service call)
  3. logs send_blocked and calls msgsend (r2)
  4. blocks until server replies (reply_blocked logged by server)
  5. logs running when msgsend returns (r5)
  6. burns cpu for the other half of its wcet
  7. sleeps for the rest of the period

this validates r1, r2, and r5 from the paper.
*/
static void *client_thread(void *arg) {
    client_arg_t *p = (client_arg_t *)arg;

    // wait for initial offset to stagger clients just like the paper does
    sleep_ms(p->offset_ms);

    // wait until main has connected to the server channel.
    // g_coid stays -1 until ConnectAttach succeeds in main.
    while (g_coid == -1) usleep(1000);

    printf("[c%d] started at priority %d, period=%dms, offset=%dms\n",
           p->id, p->priority, p->period_ms, p->offset_ms);

    client_msg_t   msg;
    server_reply_t reply;

    // fill message fields once, reuse the same message every period
    msg.type      = 0x01;
    msg.subtype   = 0x00;
    msg.client_id = p->id;

    while (g_running) {

        // r1: released at start of period, ready state
        log_event(p->id, "READY", p->priority);

        // first half of cpu work before sending the request.
        // paper says each client executes 50% of wcet before requesting.
        burn_cpu_ms(p->wcet_ms / 2);

        // r2: calling msgsend, now send_blocked
        log_event(p->id, "SEND_BLOCKED", p->priority);
        printf("[c%d] %.4f ms  send_blocked\n", p->id, elapsed_ms());

        // blocks here until the server calls msgreply.
        // while blocked we are in reply_blocked state (logged by server).
        int ret = MsgSend(g_coid, &msg, sizeof(msg), &reply, sizeof(reply));
        if (ret == -1) {
            if (!g_running) break;
            perror("MsgSend error");
            break;
        }

        // r5: server replied, running again
        log_event(p->id, "RUNNING", p->priority);
        printf("[c%d] %.4f ms  running (got reply)\n", p->id, elapsed_ms());

        // second half of cpu work after the reply
        burn_cpu_ms(p->wcet_ms / 2);

        // sleep for remainder of the period
        long long remaining = (long long)p->period_ms - (long long)p->wcet_ms;
        if (remaining > 0) sleep_ms(remaining);
    }

    printf("[c%d] done\n", p->id);
    return NULL;
}


/*
main

input: nothing
output: 0 on success

sets up and runs the experiment:
  1. starts the server thread first so it can create the channel
  2. waits for g_chid to be set by the server
  3. connects to the channel so all clients share one connection id
  4. starts all 4 client threads with correct priorities, checking
     each pthread_create for errors so we know if a thread failed
  5. waits for the full experiment duration
  6. signals all threads to stop via g_running = 0
  7. prints the sorted execution trace for analysis
*/
int main(void) {
    printf("=== setting a: baseline ipc, no inheritance, no aps ===\n");
    printf("replicating figure 2a\n\n");

    // record t=0 so all log timestamps are relative to experiment start
    clock_gettime(CLOCK_MONOTONIC, &g_start);

    // start the server thread
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
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(&server_attr);

    // wait for server to create the channel before connecting
    while (g_chid == -1) usleep(1000);

    // connect once and share the connection id across all clients.
    // ConnectAttach returns a coid that clients pass to MsgSend.
    g_coid = ConnectAttach(0, 0, g_chid, _NTO_SIDE_CHANNEL, 0);
    if (g_coid == -1) {
        perror("ConnectAttach failed");
        exit(EXIT_FAILURE);
    }
    printf("[main] connected to server channel (chid=%d, coid=%d)\n\n",
           g_chid, g_coid);

    // start all 4 client threads.
    // create them all at once and let their offset delays stagger them,
    // just like the paper does with the initial offsets 0, 6, 12, 18 ms.
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
            // print which client failed and why so we can diagnose it
            fprintf(stderr, "failed to create c%d (priority %d): %s\n",
                    i + 1, client_params[i][3], strerror(ret));
        } else {
            printf("[main] created c%d at priority %d\n",
                   i + 1, client_params[i][3]);
        }
        pthread_attr_destroy(&attr);
    }

    printf("\n[main] experiment running for %d seconds...\n\n", RUN_DURATION_S);

    sleep(RUN_DURATION_S);

    // tell all threads to stop looping
    g_running = 0;

    // give threads a moment to exit cleanly before printing
    sleep(1);

    print_log();

    ConnectDetach(g_coid);

    printf("key things to check:\n");
    printf("  r1: clients appear as READY at their release times\n");
    printf("  r2: clients go SEND_BLOCKED when they call msgsend\n");
    printf("  r5: clients go REPLY_BLOCKED then RUNNING after server replies\n");
    printf("  r4: server goes RECEIVE_BLOCKED when no messages are waiting\n");
    printf("  r7: server handles c3(241) before c4(240) before c2(239)\n");

    return EXIT_SUCCESS;
}
