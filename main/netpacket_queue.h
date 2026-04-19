#ifndef GPSP_MAIN_NETPACKET_QUEUE_H
#define GPSP_MAIN_NETPACKET_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

typedef struct {
    uint8_t *buf;
    size_t capacity;
    size_t head;
    size_t tail;
    portMUX_TYPE lock;
} netpacket_queue_t;

static inline void netpacket_queue_init(netpacket_queue_t *queue,
                                        uint8_t *storage,
                                        size_t capacity)
{
    if (!queue)
        return;

    queue->buf = storage;
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
}

static inline bool netpacket_queue_ready(const netpacket_queue_t *queue)
{
    return queue && queue->buf && queue->capacity > 1;
}

static inline size_t netpacket_queue_free_unsafe(const netpacket_queue_t *queue)
{
    if (queue->head > queue->tail)
        return queue->head - queue->tail - 1;

    return queue->capacity - queue->tail + queue->head - 1;
}

static inline size_t netpacket_queue_used_unsafe(const netpacket_queue_t *queue)
{
    return (queue->tail + queue->capacity - queue->head) % queue->capacity;
}

static inline size_t netpacket_queue_used(netpacket_queue_t *queue)
{
    size_t used = 0;

    if (!netpacket_queue_ready(queue))
        return 0;

    taskENTER_CRITICAL(&queue->lock);
    used = netpacket_queue_used_unsafe(queue);
    taskEXIT_CRITICAL(&queue->lock);
    return used;
}

static inline void netpacket_queue_write_unsafe(netpacket_queue_t *queue,
                                                const uint8_t *data,
                                                size_t len)
{
    size_t first_chunk;

    if (!len)
        return;

    first_chunk = queue->capacity - queue->tail;
    if (first_chunk > len)
        first_chunk = len;

    memcpy(queue->buf + queue->tail, data, first_chunk);
    queue->tail = (queue->tail + first_chunk) % queue->capacity;

    if (first_chunk < len) {
        size_t remaining = len - first_chunk;
        memcpy(queue->buf + queue->tail, data + first_chunk, remaining);
        queue->tail = (queue->tail + remaining) % queue->capacity;
    }
}

static inline bool netpacket_queue_enqueue2(netpacket_queue_t *queue,
                                            const void *part1,
                                            size_t part1_len,
                                            const void *part2,
                                            size_t part2_len)
{
    size_t total_len = part1_len + part2_len;
    bool ok = false;

    if (!netpacket_queue_ready(queue) || total_len >= queue->capacity)
        return false;

    taskENTER_CRITICAL(&queue->lock);
    if (netpacket_queue_free_unsafe(queue) >= total_len) {
        if (part1_len)
            netpacket_queue_write_unsafe(queue, (const uint8_t *)part1, part1_len);
        if (part2_len)
            netpacket_queue_write_unsafe(queue, (const uint8_t *)part2, part2_len);
        ok = true;
    }
    taskEXIT_CRITICAL(&queue->lock);

    return ok;
}

static inline size_t netpacket_queue_peek_contiguous(netpacket_queue_t *queue,
                                                     uint8_t **data)
{
    size_t len = 0;

    if (data)
        *data = NULL;
    if (!netpacket_queue_ready(queue))
        return 0;

    taskENTER_CRITICAL(&queue->lock);
    if (queue->head != queue->tail) {
        if (data)
            *data = queue->buf + queue->head;
        if (queue->tail > queue->head)
            len = queue->tail - queue->head;
        else
            len = queue->capacity - queue->head;
    }
    taskEXIT_CRITICAL(&queue->lock);

    return len;
}

static inline void netpacket_queue_consume(netpacket_queue_t *queue, size_t len)
{
    size_t used;

    if (!netpacket_queue_ready(queue) || !len)
        return;

    taskENTER_CRITICAL(&queue->lock);
    used = netpacket_queue_used_unsafe(queue);
    if (len > used)
        len = used;
    queue->head = (queue->head + len) % queue->capacity;
    taskEXIT_CRITICAL(&queue->lock);
}

#endif