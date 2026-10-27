# Duplex session host checks

`test_session_primitives.c` exercises the actual bounded sender queue and event-ID guard:
copied PCM, cancellation of pending old commands, BREAK-before-new-START ordering, explicit
overflow, transport failure, stale END rejection, fresh-ASR admission, MCP continuation IDs,
and fail-closed ID capacity.

`test_session_callbacks.c` includes the actual session callback implementation and links the
actual ring buffer / text / JSON helpers. Only network, credential lookup, and MCP execution
are replaced by explicit fakes. It tests old audio racing cancellation, state snapshots,
multi-event replies, and server ChatBreak flushing without resetting the active request.
It also checks a fresh frame arriving after ChatBreak but before the audio loop's next pop:
`session_pop_audio()` returns that frame and its epoch atomically, so the loop flushes only
the DAC and preserves already queued new audio.
Connection-level MCP initialize/tools/list/notifications bypass the utterance gate; tools/call
retains its admitted generation. UI clears and generation-checked text/emotion dispatch share
the state mutex, so a text callback admitted before reset cannot append after the new clear.
UI callbacks must be short, non-reentrant string-copy operations; no session or network calls.

Photo transactions share logical event ownership with the sender: an audio START..END holds
the event lane, INTERRUPT closes a still-open canceled input event, and a photo rechecks its
captured generation after waiting. A photo already sending may finish before queued BREAK
and new START, but cannot interleave their SDK event IDs. Stale waiting photos cancel without
waiting for the new utterance to finish. The audio thread never takes a transport lock.

`test_mcp_generation.c` exercises the actual photo worker and pending request slot with a fake
camera/session transaction: old capture completion retains its old generation, a pending
request cannot be overwritten silently, and only a fresh-generation photo uploads. Build it
with `core/jsonx.c`, pthread, SDK rtc headers/common/pal/mbedTLS includes, and `bsp/include`.
It includes `service/mcp.c`; do not compile that source a second time for this test.
It must NOT also compile `service/session.c` as another translation unit.

Required compile sources:

- primitives: test source + `service/audio_uplink.c` + `service/turn_guard.c`
- callbacks: test source + the above + `core/ringbuf.c` + `core/jsonx.c` + `core/text.c`

Both need C99, `_POSIX_C_SOURCE=200809L`, and pthread. Callback checks additionally include
the real agentic-kit `modules/rtc-tcp-client/include`, `modules/iot-client/include`, `pal`,
`common`, `third_party/mbedtls/include`, and `modules/atop/include` directories.

Receive admission is conservative and must be validated against device/cloud logs: a request
must reach the sender worker, then provide nonempty fresh ASR IDs before TTS is admitted.
After that, START may introduce further IDs for MCP replies. Known retired IDs are rejected;
unknown old continuation IDs after the fresh-ASR boundary cannot be perfectly attributed
with the current SDK public API. No-ID replies fail closed. Capacity is 32 IDs in an active
turn and 128 retired IDs per duplex connection; exhaustion is an explicit error requiring
reconnection. Tests do not establish live cloud compatibility or device audio quality.

The sender queue has 160 entries (about 105 KB on rv32), budgeted for two seconds of
transport jitter, up to one second of preroll, and control-command headroom.
A failed or full queue fails the conversation explicitly and requests a fresh connection;
it never blocks the audio thread waiting for a network send. Shutdown joins the one possible
in-flight transport operation, so transport timeout behavior remains an SDK acceptance item.
