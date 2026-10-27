/* Actual MCP photo worker, with camera and the session transaction replaced. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <mbedtls/base64.h>
#include "../../service/mcp.c"

static pthread_mutex_t fixture_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fixture_cond = PTHREAD_COND_INITIALIZER;
static uint64_t fixture_generation = 1;
static int captures, uploads, release_capture, responses, busy_responses, closes;
static uint64_t uploaded_generation;
static char uploaded_hint[256];

int mbedtls_base64_decode(unsigned char *dst, size_t size, size_t *out_size,
                          const unsigned char *src, size_t src_size)
{
    (void)dst; (void)size; (void)out_size; (void)src; (void)src_size;
    return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
}

int session_generation_is_current(uint64_t generation)
{
    pthread_mutex_lock(&fixture_mutex);
    int current = generation == fixture_generation;
    pthread_mutex_unlock(&fixture_mutex);
    return current;
}

void session_status_for_generation(uint64_t generation, const char *status)
{
    (void)generation; (void)status;
}

int session_send_photo_for_generation(uint64_t generation, const char *hint,
                                      const uint8_t *jpeg, size_t bytes,
                                      uint16_t width, uint16_t height)
{
    assert(jpeg && bytes && width && height);
    pthread_mutex_lock(&fixture_mutex);
    int rc = SESSION_PHOTO_CANCELED;
    if (generation == fixture_generation) {
        uploads++;
        uploaded_generation = generation;
        snprintf(uploaded_hint, sizeof(uploaded_hint), "%s", hint);
        pthread_cond_broadcast(&fixture_cond);
        rc = 0;
    }
    pthread_mutex_unlock(&fixture_mutex);
    return rc;
}

int tai_send_mcp_response(tai_ctx_t *ctx, const char *response)
{
    (void)ctx;
    pthread_mutex_lock(&fixture_mutex);
    responses++;
    busy_responses += strstr(response, "\"isError\":true") != NULL;
    pthread_cond_broadcast(&fixture_cond);
    pthread_mutex_unlock(&fixture_mutex);
    return 0;
}

static int camera_start(int width, int height)
{
    assert(width == 640 && height == 480);
    return 0;
}

static int camera_capture(const uint8_t **jpeg, size_t *bytes, int *width, int *height)
{
    static const uint8_t image[] = {0xff, 0xd8, 0xff, 0xd9};
    pthread_mutex_lock(&fixture_mutex);
    captures++;
    pthread_cond_broadcast(&fixture_cond);
    while (captures == 1 && !release_capture) pthread_cond_wait(&fixture_cond, &fixture_mutex);
    pthread_mutex_unlock(&fixture_mutex);
    *jpeg = image;
    *bytes = sizeof(image);
    *width = *height = 1;
    return 0;
}

static void camera_stop(void) { closes++; }

const bsp_t *bsp_get(void)
{
    static const bsp_t board = {.camera_start = camera_start,
        .camera_capture_jpeg = camera_capture, .camera_stop = camera_stop};
    return &board;
}

static void wait_count(int *counter, int minimum)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&fixture_mutex);
    while (*counter < minimum)
        assert(!pthread_cond_timedwait(&fixture_cond, &fixture_mutex, &until));
    pthread_mutex_unlock(&fixture_mutex);
}

static void photo(tai_ctx_t *ctx, uint64_t generation, const char *hint)
{
    char json[512];
    int size = snprintf(json, sizeof(json),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"take_photo\",\"arguments\":{\"hint\":\"%s\"}}}", hint);
    assert(size > 0 && (size_t)size < sizeof(json));
    mcp_handle_generation(ctx, json, generation);
}

int main(void)
{
    int context;
    assert(mcp_start((tai_ctx_t *)&context, NULL) == 0);
    photo((tai_ctx_t *)&context, 1, "active old request");
    wait_count(&captures, 1);
    photo((tai_ctx_t *)&context, 1, "pending old request");
    photo((tai_ctx_t *)&context, 2, "must not overwrite pending");
    assert(responses == 3 && busy_responses == 1);
    pthread_mutex_lock(&g_photo_mutex);
    assert(g_photo_pending && g_pending_photo.generation == 1);
    assert(!strcmp(g_pending_photo.hint, "pending old request"));
    pthread_mutex_unlock(&g_photo_mutex);

    /* A new START while the old camera is still capturing must not relabel its
     * eventual result. The queued old request must also remain old. */
    pthread_mutex_lock(&fixture_mutex);
    fixture_generation = 2;
    release_capture = 1;
    pthread_cond_broadcast(&fixture_cond);
    pthread_mutex_unlock(&fixture_mutex);

    /* Wait for the worker to consume the old pending slot. It is rejected by
     * session_generation_is_current before another camera acquisition. */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    for (;;) {
        pthread_mutex_lock(&g_photo_mutex);
        int pending = g_photo_pending;
        pthread_mutex_unlock(&g_photo_mutex);
        if (!pending) break;
        struct timespec now, pause = {0, 1000000};
        clock_gettime(CLOCK_REALTIME, &now);
        assert(now.tv_sec < until.tv_sec ||
               (now.tv_sec == until.tv_sec && now.tv_nsec < until.tv_nsec));
        nanosleep(&pause, NULL);
    }
    photo((tai_ctx_t *)&context, 2, "fresh request");
    wait_count(&uploads, 1);
    mcp_stop();
    assert(captures == 2 && uploads == 1 && uploaded_generation == 2);
    assert(!strcmp(uploaded_hint, "fresh request") && closes == 1);
    puts("MCP generation: stale capture canceled, pending slot preserved, fresh photo uploaded");
    return 0;
}
