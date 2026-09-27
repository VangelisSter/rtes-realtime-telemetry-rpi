#include "circular_buffer.h"
#include <stdlib.h>
#include <string.h>

int queue_init(circular_queue_t *q, size_t capacity) {
    if (!q || capacity == 0) {
        return -1;
    }

    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->dropped_count = 0;
    q->shutdown = 0;

    q->items = (queue_item_t *)calloc(capacity, sizeof(queue_item_t));
    if (!q->items) {
        return -1;
    }

    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        free(q->items);
        q->items = NULL;
        return -1;
    }

    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->mutex);
        free(q->items);
        q->items = NULL;
        return -1;
    }

    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->mutex);
        free(q->items);
        q->items = NULL;
        return -1;
    }

    return 0;
}

void queue_destroy(circular_queue_t *q) {
    if (!q) {
        return;
    }

    pthread_mutex_lock(&q->mutex);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);

    if (q->items) {
        free(q->items);
        q->items = NULL;
    }
}

int queue_push(circular_queue_t *q, const char *msg, size_t len) {
    if (!q || !msg || len == 0) {
        return -1;
    }

    pthread_mutex_lock(&q->mutex);

    if (q->shutdown) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    /* Clamp message length to fit in fixed preallocated slot */
    if (len >= MAX_MSG_LEN) {
        len = MAX_MSG_LEN - 1;
    }

    /* If queue is completely full, drop the oldest entry to maintain real-time flow */
    if (q->count == q->capacity) {
        q->tail = (q->tail + 1) % q->capacity;
        q->count--;
        q->dropped_count++;
    }

    /* Copy payload into preallocated slot */
    memcpy(q->items[q->head].data, msg, len);
    q->items[q->head].data[len] = '\0';
    q->items[q->head].len = len;

    q->head = (q->head + 1) % q->capacity;
    q->count++;

    /* Signal consumer that a new item is available */
    pthread_cond_signal(&q->not_empty);

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int queue_pop(circular_queue_t *q, char *out_buf, size_t out_buf_size, size_t *out_len) {
    if (!q || !out_buf || out_buf_size == 0) {
        return -1;
    }

    pthread_mutex_lock(&q->mutex);

    /* Wait until data is available or shutdown is requested */
    while (q->count == 0 && !q->shutdown) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    /* If queue is empty and shutting down, exit */
    if (q->count == 0 && q->shutdown) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    queue_item_t *item = &q->items[q->tail];
    size_t copy_len = item->len;
    if (copy_len >= out_buf_size) {
        copy_len = out_buf_size - 1;
    }

    memcpy(out_buf, item->data, copy_len);
    out_buf[copy_len] = '\0';

    if (out_len) {
        *out_len = copy_len;
    }

    q->tail = (q->tail + 1) % q->capacity;
    q->count--;

    pthread_cond_signal(&q->not_full);

    pthread_mutex_unlock(&q->mutex);
    return 1;
}

double queue_get_occupancy_pct(circular_queue_t *q) {
    if (!q || q->capacity == 0) {
        return 0.0;
    }

    pthread_mutex_lock(&q->mutex);
    double pct = ((double)q->count / (double)q->capacity) * 100.0;
    pthread_mutex_unlock(&q->mutex);

    return pct;
}

size_t queue_get_dropped_count(circular_queue_t *q) {
    if (!q) {
        return 0;
    }

    pthread_mutex_lock(&q->mutex);
    size_t count = q->dropped_count;
    pthread_mutex_unlock(&q->mutex);

    return count;
}

void queue_shutdown(circular_queue_t *q) {
    if (!q) {
        return;
    }

    pthread_mutex_lock(&q->mutex);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}
