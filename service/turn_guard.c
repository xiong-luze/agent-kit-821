#include "turn_guard.h"
#include <errno.h>
#include <string.h>

static int contains(char ids[][TURN_GUARD_ID_SIZE], unsigned count, const char *id)
{
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(ids[i], id)) return 1;
    return 0;
}

static void retire(turn_guard_t *guard)
{
    for (unsigned i = 0; i < guard->current_count; i++) {
        if (contains(guard->retired, guard->retired_count, guard->current[i])) continue;
        if (guard->retired_count == TURN_GUARD_RETIRED_IDS) {
            guard->error = -ENOSPC;
            break;
        }
        strcpy(guard->retired[guard->retired_count++], guard->current[i]);
    }
    guard->current_count = 0;
}

void turn_guard_init(turn_guard_t *guard)
{
    memset(guard, 0, sizeof(*guard));
}

uint64_t turn_guard_begin(turn_guard_t *guard)
{
    retire(guard);
    guard->request_sent = guard->ready = 0;
    return ++guard->generation;
}

uint64_t turn_guard_interrupt(turn_guard_t *guard)
{
    return turn_guard_begin(guard);
}

void turn_guard_request_sent(turn_guard_t *guard, uint64_t generation)
{
    if (generation == guard->generation && !guard->error) guard->request_sent = 1;
}

int turn_guard_accept(turn_guard_t *guard, const char *id, turn_guard_kind_t kind)
{
    if (guard->error || !guard->request_sent || !id || !id[0] ||
        strlen(id) >= TURN_GUARD_ID_SIZE ||
        contains(guard->retired, guard->retired_count, id)) return 0;
    if (!guard->ready) {
        if (kind != TURN_GUARD_ASR) return 0;
        guard->ready = 1;
    }
    int known = contains(guard->current, guard->current_count, id);
    if (!known) {
        if (kind != TURN_GUARD_ASR && kind != TURN_GUARD_START) return 0;
        if (guard->current_count == TURN_GUARD_CURRENT_IDS) {
            guard->error = -ENOSPC;
            return 0;
        }
        strcpy(guard->current[guard->current_count++], id);
    }
    return 1;
}

int turn_guard_error(const turn_guard_t *guard) { return guard->error; }
