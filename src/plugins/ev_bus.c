/**
 * @file    ev_bus.c
 * @brief   通用观察者总线实现
 */
#include "evkit/ev_bus.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    ev_bus_cb cb;
    const void *topic;
    void *payload;
    void *user_data;
} ev_bus_delivery_t;

static void ev_bus_on_delivery(void *user_data)
{
    ev_bus_delivery_t *d = user_data;
    if (d->cb != NULL) {
        d->cb(d->topic, d->payload, d->user_data);
    }
    /* delivery 由 post_full 的 free_fn 释放（含 deinit 丢弃路径） */
}

static void ev_bus_free_delivery(void *user_data)
{
    free(user_data);
}

int ev_bus_match_u32(const void *topic, const void *match_arg, void *user_data)
{
    (void)user_data;
    if (topic == NULL || match_arg == NULL) {
        return 0;
    }
    return (*(const uint32_t *)topic == *(const uint32_t *)match_arg) ? 1 : 0;
}

int ev_bus_init(ev_bus_t *bus, ev_loop_t *owner_loop)
{
    if (bus == NULL || owner_loop == NULL) {
        return -1;
    }
    memset(bus, 0, sizeof(*bus));
    bus->owner_loop = owner_loop;
    bus->next_sub_id = 1;
    return 0;
}

void ev_bus_deinit(ev_bus_t *bus)
{
    if (bus == NULL) {
        return;
    }
    memset(bus->subs, 0, sizeof(bus->subs));
    bus->sub_count = 0;
    bus->owner_loop = NULL;
}

int ev_bus_subscribe(ev_bus_t *bus, ev_bus_mode_t mode, ev_loop_t *target_loop, ev_bus_cb cb, void *user_data, ev_bus_match_fn match, const void *match_arg)
{
    if (bus == NULL || bus->owner_loop == NULL || cb == NULL || match == NULL) {
        return -1;
    }
    if (mode != EV_BUS_SYNC && mode != EV_BUS_POST) {
        return -1;
    }
    if (mode == EV_BUS_POST && target_loop == NULL) {
        return -1;
    }
    if (bus->sub_count >= EV_BUS_SUB_MAX) {
        return -1;
    }
    if (bus->next_sub_id <= 0) {
        return -1;
    }

    ev_bus_sub_t *s = &bus->subs[bus->sub_count];
    s->sub_id = bus->next_sub_id++;
    s->mode = mode;
    s->target_loop = (mode == EV_BUS_POST) ? target_loop : NULL;
    s->cb = cb;
    s->user_data = user_data;
    s->match = match;
    s->match_arg = match_arg;
    bus->sub_count++;
    return s->sub_id;
}

int ev_bus_unsubscribe(ev_bus_t *bus, int sub_id)
{
    if (bus == NULL || sub_id <= 0) {
        return -1;
    }
    for (int i = 0; i < bus->sub_count; i++) {
        if (bus->subs[i].sub_id != sub_id) {
            continue;
        }
        bus->subs[i] = bus->subs[bus->sub_count - 1];
        memset(&bus->subs[bus->sub_count - 1], 0, sizeof(bus->subs[0]));
        bus->sub_count--;
        return 0;
    }
    return -1;
}

int ev_bus_publish(ev_bus_t *bus, const void *topic, void *payload)
{
    if (bus == NULL || bus->owner_loop == NULL) {
        return -1;
    }

    /* 先整表拷贝再 match/分发：match 或 SYNC 回调里增删不影响本次迭代 */
    ev_bus_sub_t snaps[EV_BUS_SUB_MAX];
    int nsnap = bus->sub_count;
    if (nsnap > EV_BUS_SUB_MAX) {
        nsnap = EV_BUS_SUB_MAX;
    }
    if (nsnap > 0) {
        memcpy(snaps, bus->subs, (size_t)nsnap * sizeof(snaps[0]));
    }

    int n = 0;
    for (int i = 0; i < nsnap; i++) {
        ev_bus_sub_t *s = &snaps[i];
        if (s->sub_id <= 0 || s->cb == NULL || s->match == NULL) {
            continue;
        }
        if (!s->match(topic, s->match_arg, s->user_data)) {
            continue;
        }
        switch (s->mode) {
        case EV_BUS_SYNC:
            s->cb(topic, payload, s->user_data);
            n++;
            break;
        case EV_BUS_POST: {
            ev_bus_delivery_t *d = calloc(1, sizeof(*d));
            if (d == NULL) {
                bus->post_dropped++;
                break;
            }
            d->cb = s->cb;
            d->topic = topic;
            d->payload = payload;
            d->user_data = s->user_data;
            if (ev_loop_post_full(s->target_loop, ev_bus_on_delivery, d, ev_bus_free_delivery) != 0) {
                free(d);
                bus->post_dropped++;
                break;
            }
            n++;
            break;
        }
        default:
            break;
        }
    }
    return n;
}
