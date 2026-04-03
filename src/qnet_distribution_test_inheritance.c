/*
qnet_distribution_test_inheritance.c

Same distributed QNET latency experiment as qnet_distribution_test.c,
but with priority inheritance enabled.

Difference from the no-inheritance version:
- server channel is created with ChannelCreate(0)
- while serving a client request, the server may inherit the client's
  priority according to QNX synchronous message-passing semantics

Usage is the same:
  qnet_distribution_test_inheritance server
  qnet_distribution_test_inheritance client <server_node_name> <server_pid> <server_chid>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <float.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/neutrino.h>
#include <sys/netmgr.h>

#define SERVER_PRIORITY 238
#define CLIENT_PRIORITY 236
#define ITERATIONS      10000
#define MAX_MSG_SIZE    2048
#define NS_TO_US(ns)    ((double)(ns) / 1000.0)

static const int msg_sizes[] = {96, 128, 256, 512, 1024, 1536, 2048};
#define NUM_SIZES (sizeof(msg_sizes) / sizeof(msg_sizes[0]))

typedef struct {
    double min_us;
    double max_us;
    double avg_us;
    int    msg_size;
    int    iterations;
} latency_result_t;

static latency_result_t g_results[NUM_SIZES];

static void set_fifo_priority(int prio) {
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        perror("pthread_setschedparam");
    }
}

static long long diff_ns(const struct timespec *a, const struct timespec *b) {
    return ((long long)(b->tv_sec - a->tv_sec) * 1000000000LL)
         + ((long long)(b->tv_nsec - a->tv_nsec));
}

static int run_server(void) {
    set_fifo_priority(SERVER_PRIORITY);

    int chid = ChannelCreate(0);
    if (chid == -1) {
        perror("ChannelCreate");
        return EXIT_FAILURE;
    }

    printf("[server] qnet distribution server started\n");
    printf("[server] pid=%d chid=%d priority=%d inheritance=enabled\n",
           getpid(), chid, SERVER_PRIORITY);
    printf("[server] ready to receive remote MsgSend() calls\n");

    char recv_buf[MAX_MSG_SIZE];
    struct _msg_info info;

    while (1) {
        int rcvid = MsgReceive(chid, recv_buf, sizeof(recv_buf), &info);
        if (rcvid == -1) {
            perror("MsgReceive");
            break;
        }

        if (info.msglen == 4 && memcmp(recv_buf, "STOP", 4) == 0) {
            MsgReply(rcvid, EOK, recv_buf, info.msglen);
            break;
        }

        if (MsgReply(rcvid, EOK, recv_buf, info.msglen) == -1) {
            perror("MsgReply");
            break;
        }
    }

    ChannelDestroy(chid);
    return EXIT_SUCCESS;
}

static int run_client(const char *node_name, pid_t server_pid, int server_chid) {
    set_fifo_priority(CLIENT_PRIORITY);

    int nd = netmgr_strtond(node_name, NULL);
    if (nd == -1) {
        perror("netmgr_strtond");
        fprintf(stderr, "could not resolve remote node '%s'\n", node_name);
        return EXIT_FAILURE;
    }

    int coid = ConnectAttach(nd, server_pid, server_chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        perror("ConnectAttach");
        return EXIT_FAILURE;
    }

    printf("[client] connected to node=%s nd=%d pid=%d chid=%d\n",
           node_name, nd, (int)server_pid, server_chid);
    printf("[client] running %d iterations per message size\n\n", ITERATIONS);

    char send_buf[MAX_MSG_SIZE];
    char recv_buf[MAX_MSG_SIZE];
    for (int i = 0; i < MAX_MSG_SIZE; i++) {
        send_buf[i] = (char)(i & 0xFF);
    }

    for (int i = 0; i < 100; i++) {
        if (MsgSend(coid, send_buf, 96, recv_buf, 96) == -1) {
            perror("MsgSend warmup");
            ConnectDetach(coid);
            return EXIT_FAILURE;
        }
    }

    for (int s = 0; s < (int)NUM_SIZES; s++) {
        int size = msg_sizes[s];
        double min_us = DBL_MAX;
        double max_us = 0.0;
        double total_us = 0.0;

        printf("[client] testing %d bytes...\n", size);

        for (int i = 0; i < ITERATIONS; i++) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            if (MsgSend(coid, send_buf, size, recv_buf, size) == -1) {
                perror("MsgSend");
                ConnectDetach(coid);
                return EXIT_FAILURE;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);

            double us = NS_TO_US(diff_ns(&t0, &t1));
            if (us < min_us) min_us = us;
            if (us > max_us) max_us = us;
            total_us += us;
        }

        g_results[s].msg_size = size;
        g_results[s].iterations = ITERATIONS;
        g_results[s].min_us = min_us;
        g_results[s].avg_us = total_us / ITERATIONS;
        g_results[s].max_us = max_us;
    }

    printf("\n========== qnet distributed latency (inheritance) ==========\n");
    printf("%-10s %-12s %-12s %-12s %-10s\n",
           "size(B)", "min(us)", "avg(us)", "max(us)", "iters");
    for (int i = 0; i < (int)NUM_SIZES; i++) {
        printf("%-10d %-12.2f %-12.2f %-12.2f %-10d\n",
               g_results[i].msg_size,
               g_results[i].min_us,
               g_results[i].avg_us,
               g_results[i].max_us,
               g_results[i].iterations);
    }
    printf("============================================================\n");

    if (MsgSend(coid, "STOP", 4, recv_buf, 4) == -1) {
        perror("MsgSend STOP");
    }

    ConnectDetach(coid);
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
                "usage:\n"
                "  %s server\n"
                "  %s client <server_node_name> <server_pid> <server_chid>\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "server") == 0) {
        return run_server();
    }

    if (strcmp(argv[1], "client") == 0) {
        if (argc != 5) {
            fprintf(stderr,
                    "client mode requires: <server_node_name> <server_pid> <server_chid>\n");
            return EXIT_FAILURE;
        }
        return run_client(argv[2], (pid_t)atoi(argv[3]), atoi(argv[4]));
    }

    fprintf(stderr, "unknown mode '%s'\n", argv[1]);
    return EXIT_FAILURE;
}
