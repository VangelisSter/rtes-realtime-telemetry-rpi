#ifndef CIRCULAR_BUFFER_H
#define CIRCULAR_BUFFER_H

#include <stddef.h>
#include <pthread.h>

#define MAX_MSG_LEN 16384
#define DEFAULT_QUEUE_CAPACITY 2048

typedef struct {
    char data[MAX_MSG_LEN];
    size_t len;
} queue_item_t;

typedef struct {
    queue_item_t *items;
    size_t capacity;
    size_t head;          /* Index to push next message */
    size_t tail;          /* Index to pop next message */
    size_t count;         /* Current number of queued messages */
    size_t dropped_count; /* Messages dropped due to buffer overflow */
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int shutdown;         /* 1 if queue is shutting down */
} circular_queue_t;

/**
 * Initialize the circular buffer with the specified capacity.
 * Preallocates memory for items to avoid heap allocation during runtime.
 * Returns 0 on success, -1 on allocation failure.
 */
int queue_init(circular_queue_t *q, size_t capacity);

/**
 * Clean up and free circular buffer resources.
 */
void queue_destroy(circular_queue_t *q);

/**
 * Push a message into the circular buffer.
 * If the buffer is full, it drops the oldest message (overwrites) to maintain
 * real-time latency and increments dropped_count.
 * Signals waiting consumer via condition variable.
 * Returns 0 on success, -1 if message too large or queue shutting down.
 */
int queue_push(circular_queue_t *q, const char *msg, size_t len);

/**
 * Pop a message from the circular buffer.
 * Blocks on not_empty condition variable if queue is empty.
 * Returns 1 if an item was successfully popped,
 * 0 if queue is empty and shutting down,
 * -1 on error.
 */
int queue_pop(circular_queue_t *q, char *out_buf, size_t out_buf_size, size_t *out_len);

/**
 * Thread-safe query for the current buffer occupancy percentage [0.0 - 100.0%].
 */
double queue_get_occupancy_pct(circular_queue_t *q);

/**
 * Get the total number of dropped messages since initialization.
 */
size_t queue_get_dropped_count(circular_queue_t *q);

/**
 * Signals the queue to shutdown, unblocking any waiting threads.
 */
void queue_shutdown(circular_queue_t *q);

#endif /* CIRCULAR_BUFFER_H */
