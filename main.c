#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <libwebsockets.h>

#include "circular_buffer.h"
#include "cJSON.h"
#include "cpu_stats.h"

#define JETSTREAM_HOST "jetstream1.us-east.bsky.network"
#define JETSTREAM_PORT 443
#define JETSTREAM_PATH "/subscribe?wantedCollections=app.bsky.feed.post"

/* Global control and sync */
static volatile sig_atomic_t g_running = 1;
static circular_queue_t g_queue;

/* Global Message Counters (Protected by g_counters_mutex) */
static pthread_mutex_t g_counters_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_commit_count = 0;
static unsigned long g_identity_count = 0;
static unsigned long g_account_count = 0;
static unsigned long g_info_count = 0;

/* Producer / WebSocket State */
static struct lws_context *g_lws_context = NULL;
static struct lws *g_wsi = NULL;
static volatile int g_connected = 0;
static FILE *g_log_fp = NULL;

/* Session data buffer for fragment reassembly */
struct per_session_data__jetstream {
    char rx_buf[MAX_MSG_LEN];
    size_t rx_len;
};

/* Signal handler for graceful shutdown: sets atomic flag and interrupts network service */
static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (g_lws_context) {
        lws_cancel_service(g_lws_context);
    }
}

/* ========================================================================= */
/* THREAD 1: PRODUCER (libwebsockets callback & loop)                        */
/* ========================================================================= */
static int callback_jetstream(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
    struct per_session_data__jetstream *pss = (struct per_session_data__jetstream *)user;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            g_connected = 1;
            fprintf(stderr, "[PRODUCER] Connected to Bluesky Jetstream WebSocket.\n");
            if (pss) {
                pss->rx_len = 0;
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (!pss) break;

            /* Accumulate chunks in case frame arrives fragmented */
            if (pss->rx_len + len < MAX_MSG_LEN) {
                memcpy(pss->rx_buf + pss->rx_len, in, len);
                pss->rx_len += len;
            } else {
                /* Frame too large for slot; drop fragment to prevent overflow */
                pss->rx_len = 0;
            }

            /* Check if this is the final fragment of the frame */
            if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
                if (pss->rx_len > 0) {
                    pss->rx_buf[pss->rx_len] = '\0';
                    queue_push(&g_queue, pss->rx_buf, pss->rx_len);
                    pss->rx_len = 0;
                }
            }
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            g_connected = 0;
            g_wsi = NULL;
            fprintf(stderr, "[PRODUCER] Connection error: %s\n", in ? (char *)in : "(unknown)");
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            g_connected = 0;
            g_wsi = NULL;
            fprintf(stderr, "[PRODUCER] Connection closed. Will attempt reconnect.\n");
            break;

        case LWS_CALLBACK_WSI_DESTROY:
            if (g_wsi == wsi) {
                g_wsi = NULL;
            }
            break;

        default:
            break;
    }

    return 0;
}

static const struct lws_protocols protocols[] = {
    {
        "jetstream-client",
        callback_jetstream,
        sizeof(struct per_session_data__jetstream),
        MAX_MSG_LEN,
        0, NULL, 0
    },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static void *producer_thread_func(void *arg) {
    (void)arg;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.gid = -1;
    info.uid = -1;

    g_lws_context = lws_create_context(&info);
    if (!g_lws_context) {
        fprintf(stderr, "[PRODUCER] Failed to create libwebsockets context.\n");
        g_running = 0;
        queue_shutdown(&g_queue);
        return NULL;
    }

    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = g_lws_context;
    ccinfo.address = JETSTREAM_HOST;
    ccinfo.port = JETSTREAM_PORT;
    ccinfo.path = JETSTREAM_PATH;
    ccinfo.host = JETSTREAM_HOST;
    ccinfo.origin = JETSTREAM_HOST;
    ccinfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    ccinfo.protocol = protocols[0].name;
    ccinfo.pwsi = &g_wsi;

    time_t last_connect_time = 0;

    while (g_running) {
        if (!g_connected && !g_wsi) {
            time_t now = time(NULL);
            if (now - last_connect_time >= 3) {
                last_connect_time = now;
                fprintf(stderr, "[PRODUCER] Connecting to wss://%s:%d%s...\n",
                        JETSTREAM_HOST, JETSTREAM_PORT, JETSTREAM_PATH);
                g_wsi = lws_client_connect_via_info(&ccinfo);
            }
        }

        /* Service network events with 50ms timeout */
        lws_service(g_lws_context, 50);
    }

    /* Unblock consumer thread on shutdown */
    queue_shutdown(&g_queue);

    if (g_lws_context) {
        lws_context_destroy(g_lws_context);
        g_lws_context = NULL;
    }

    return NULL;
}

/* ========================================================================= */
/* THREAD 2: CONSUMER (Event-driven JSON parsing & classification)           */
/* ========================================================================= */
static void *consumer_thread_func(void *arg) {
    (void)arg;
    char pop_buf[MAX_MSG_LEN];
    size_t pop_len = 0;

    while (g_running) {
        int ret = queue_pop(&g_queue, pop_buf, sizeof(pop_buf), &pop_len);
        if (ret <= 0) {
            /* Queue is empty and shutting down */
            if (!g_running) break;
            continue;
        }

        /* Parse JSON using cJSON */
        cJSON *root = cJSON_ParseWithLength(pop_buf, pop_len);
        if (root) {
            cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
            if (cJSON_IsString(kind) && (kind->valuestring != NULL)) {
                const char *val = kind->valuestring;
                pthread_mutex_lock(&g_counters_mutex);
                if (strcmp(val, "commit") == 0) {
                    g_commit_count++;
                } else if (strcmp(val, "identity") == 0) {
                    g_identity_count++;
                } else if (strcmp(val, "account") == 0) {
                    g_account_count++;
                } else {
                    g_info_count++;
                }
                pthread_mutex_unlock(&g_counters_mutex);
            }
            cJSON_Delete(root);
        }
        /* NOTE: Strictly NO printf or file writes allowed in this thread */
    }

    return NULL;
}

/* ========================================================================= */
/* THREAD 3: MONITOR / LOGGER (Strictly Periodic @ 1.000000000s)             */
/* ========================================================================= */
static void *monitor_thread_func(void *arg) {
    (void)arg;

    /* Attempt to set real-time priority (SCHED_FIFO) for minimal jitter */
    struct sched_param param;
    param.sched_priority = 80;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0) {
        fprintf(stderr, "[MONITOR] Real-time scheduling SCHED_FIFO (priority 80) enabled.\n");
    } else {
        fprintf(stderr, "[MONITOR] Running with standard SCHED_OTHER scheduling (run with sudo/CAP_SYS_NICE for SCHED_FIFO).\n");
    }

    struct timespec next_wake;
    clock_gettime(CLOCK_MONOTONIC, &next_wake);

    while (g_running) {
        /* Advance next wakeup time by exactly 1.000000000 second to avoid clock drift */
        next_wake.tv_sec += 1;

        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wake, NULL) != 0) {
            if (!g_running) {
                break;
            }
        }
        if (!g_running) {
            break;
        }

        /* 1. Precise Realtime Timestamp */
        struct timespec ts_real;
        clock_gettime(CLOCK_REALTIME, &ts_real);

        /* 2. Snapshot and reset the 1-second message counters under mutex */
        pthread_mutex_lock(&g_counters_mutex);
        unsigned long commit_snap = g_commit_count;
        unsigned long identity_snap = g_identity_count;
        unsigned long account_snap = g_account_count;
        unsigned long info_snap = g_info_count;
        g_commit_count = 0;
        g_identity_count = 0;
        g_account_count = 0;
        g_info_count = 0;
        pthread_mutex_unlock(&g_counters_mutex);

        /* 3. Compute circular buffer occupancy percentage */
        double buffer_pct = queue_get_occupancy_pct(&g_queue);

        /* 4. Compute CPU percentage via /proc/stat */
        double cpu_pct = cpu_stats_get_usage_pct();

        /* 5. Append to log file:
              Seconds,Nanoseconds,Commit_Count,Identity_Count,Account_Count,Info_Count,Buffer_Occupancy_Pct,CPU_Pct */
        if (g_log_fp) {
            fprintf(g_log_fp, "%lld,%09ld,%lu,%lu,%lu,%lu,%.2f,%.2f\n",
                    (long long)ts_real.tv_sec, ts_real.tv_nsec,
                    commit_snap, identity_snap, account_snap, info_snap,
                    buffer_pct, cpu_pct);
            fflush(g_log_fp);
        }
    }

    return NULL;
}

/* ========================================================================= */
/* MAIN ENTRY POINT                                                          */
/* ========================================================================= */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("============================================================\n");
    printf(" Real-Time Embedded Telemetry System (Bluesky Jetstream)   \n");
    printf(" Target: Raspberry Pi Zero W (Raspberry Pi OS Lite 32-bit)  \n");
    printf("============================================================\n");

    /* Install signal handlers for clean shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Initialize CPU stats baseline */
    if (cpu_stats_init() != 0) {
        fprintf(stderr, "[WARN] Could not read /proc/stat (CPU %% will report 0.0 on non-Linux platforms).\n");
    }

    /* Initialize bounded circular queue */
    if (queue_init(&g_queue, DEFAULT_QUEUE_CAPACITY) != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize circular buffer.\n");
        return EXIT_FAILURE;
    }
    printf("[INIT] Circular buffer initialized with %d slots of %d bytes.\n",
           DEFAULT_QUEUE_CAPACITY, MAX_MSG_LEN);

    /* Create a new log file for each run with timestamp indicator */
    char log_filename[256];
    time_t t_now = time(NULL);
    struct tm tm_now;
    localtime_r(&t_now, &tm_now);
    snprintf(log_filename, sizeof(log_filename),
             "metrics_log_%04d%02d%02d_%02d%02d%02d.txt",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

    g_log_fp = fopen(log_filename, "w");
    if (!g_log_fp) {
        fprintf(stderr, "[ERROR] Could not open %s for writing.\n", log_filename);
        queue_destroy(&g_queue);
        return EXIT_FAILURE;
    }

    /* Write CSV header */
    fprintf(g_log_fp, "Seconds,Nanoseconds,Commit_Count,Identity_Count,Account_Count,Info_Count,Buffer_Occupancy_Pct,CPU_Pct\n");
    fflush(g_log_fp);

    /* Update convenience symlink metrics_log.txt to point to the active log file */
    unlink("metrics_log.txt");
    if (symlink(log_filename, "metrics_log.txt") == 0) {
        printf("[INIT] Logging metrics to %s (symlinked as metrics_log.txt)\n", log_filename);
    } else {
        printf("[INIT] Logging metrics to %s\n", log_filename);
    }

    /* Launch Threads */
    pthread_t producer_tid, consumer_tid, monitor_tid;

    if (pthread_create(&consumer_tid, NULL, consumer_thread_func, NULL) != 0) {
        fprintf(stderr, "[ERROR] Failed to spawn Consumer thread.\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&monitor_tid, NULL, monitor_thread_func, NULL) != 0) {
        fprintf(stderr, "[ERROR] Failed to spawn Monitor thread.\n");
        return EXIT_FAILURE;
    }

    if (pthread_create(&producer_tid, NULL, producer_thread_func, NULL) != 0) {
        fprintf(stderr, "[ERROR] Failed to spawn Producer thread.\n");
        return EXIT_FAILURE;
    }

    printf("[STATUS] All 3 threads spawned successfully. Running... (Press Ctrl+C to terminate)\n");

    /* Wait for shutdown */
    pthread_join(producer_tid, NULL);
    pthread_join(consumer_tid, NULL);
    pthread_join(monitor_tid, NULL);

    printf("\n[SHUTDOWN] Cleaning up resources...\n");

    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }

    queue_destroy(&g_queue);
    pthread_mutex_destroy(&g_counters_mutex);

    printf("[SHUTDOWN] Terminated cleanly. Total dropped buffer messages: %zu\n",
           queue_get_dropped_count(&g_queue));
    return EXIT_SUCCESS;
}
