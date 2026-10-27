/* Event-ID filter for opt-in duplex mode. Caller owns synchronization. */
#ifndef TURN_GUARD_H
#define TURN_GUARD_H
#include <stdint.h>

#define TURN_GUARD_ID_SIZE 64
#define TURN_GUARD_CURRENT_IDS 32
#define TURN_GUARD_RETIRED_IDS 128
typedef enum {
    TURN_GUARD_ASR, TURN_GUARD_START, TURN_GUARD_MEDIA, TURN_GUARD_END
} turn_guard_kind_t;
typedef struct {
    char current[TURN_GUARD_CURRENT_IDS][TURN_GUARD_ID_SIZE];
    char retired[TURN_GUARD_RETIRED_IDS][TURN_GUARD_ID_SIZE];
    unsigned current_count, retired_count;
    uint64_t generation;
    int request_sent, ready, error;
} turn_guard_t;

void turn_guard_init(turn_guard_t *guard);
uint64_t turn_guard_begin(turn_guard_t *guard);
uint64_t turn_guard_interrupt(turn_guard_t *guard);
void turn_guard_request_sent(turn_guard_t *guard, uint64_t generation);
int turn_guard_accept(turn_guard_t *guard, const char *event_id, turn_guard_kind_t kind);
int turn_guard_error(const turn_guard_t *guard);

/* Admission is intentionally conservative: each new request must be sent, then
 * produce an ASR with a nonempty, non-retired ID. Only then may response STARTs
 * introduce more IDs (MCP continuation); END only applies to an observed ID.
 * Unknown old continuation IDs after this boundary cannot be attributed with
 * the current SDK's public API. Do not claim strict causal correlation. Saturated
 * ID sets fail closed and require reconnect instead of evicting stale IDs. */
#endif
