#include "hidway_motion.h"

static int32_t sat_add(int32_t a, int32_t b)
{
    int64_t s = (int64_t)a + b;
    if (s > HIDWAY_MOTION_CAP)
        return HIDWAY_MOTION_CAP;
    if (s < -HIDWAY_MOTION_CAP)
        return -HIDWAY_MOTION_CAP;
    return (int32_t)s;
}

static int32_t take_axis(int32_t *acc, int32_t limit)
{
    int32_t v = *acc;
    if (v > limit)
        v = limit;
    else if (v < -limit)
        v = -limit;
    *acc -= v;
    return v;
}

void hidway_motion_clear(hidway_motion_t *m)
{
    m->x = m->y = m->wheel = m->pan = 0;
}

void hidway_motion_add(hidway_motion_t *m, int32_t dx, int32_t dy, int32_t wheel, int32_t pan)
{
    m->x = sat_add(m->x, dx);
    m->y = sat_add(m->y, dy);
    m->wheel = sat_add(m->wheel, wheel);
    m->pan = sat_add(m->pan, pan);
}

bool hidway_motion_pending(const hidway_motion_t *m)
{
    return m->x || m->y || m->wheel || m->pan;
}

void hidway_motion_take(hidway_motion_t *m, int32_t xy_limit, int32_t wheel_limit, hidway_motion_t *out)
{
    out->x = take_axis(&m->x, xy_limit);
    out->y = take_axis(&m->y, xy_limit);
    out->wheel = take_axis(&m->wheel, wheel_limit);
    out->pan = take_axis(&m->pan, wheel_limit);
}

void hidway_motion_give_back(hidway_motion_t *m, const hidway_motion_t *chunk)
{
    hidway_motion_add(m, chunk->x, chunk->y, chunk->wheel, chunk->pan);
}
