#include "decode_workers.h"

#include <pthread.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "common/decode_psram.h"
#include "esp_pthread.h"
#else
#define DECODE_PSRAM_BSS
#endif

// The helper's state. Everything but the attempts and the waterfall is read and written under `lock`.
// The helper's decode reads the candidate, the config, the monitor and the waterfall, while the caller's
// commits may be subtracting from that waterfall: a decode whose bins a commit since its stamp lowered
// is discarded at its commit (decode_core.h), so what it read there is never used.
struct decode_helper {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;  // a job was posted
    pthread_cond_t done;  // a job finished
    decode_core_t* core;
    int job;      // the candidate posted for the helper, -1 for none
    int running;  // the candidate the helper is decoding, -1 for none
    uint8_t finished[DECODE_MAX_CANDIDATES];  // this search's candidates the helper has decoded
    decode_core_attempt_t attempt[DECODE_MAX_CANDIDATES];
};

static DECODE_PSRAM_BSS decode_helper_t s_helper;
static int s_started;

static void* helper_main(void* argument)
{
    decode_helper_t* h = (decode_helper_t*)argument;
    pthread_mutex_lock(&h->lock);
    for (;;) {
        while (h->job < 0) pthread_cond_wait(&h->wake, &h->lock);
        const int k = h->job;
        decode_core_t* core = h->core;
        h->job = -1;
        h->running = k;
        pthread_mutex_unlock(&h->lock);
        decode_core_decode(core, k, &h->attempt[k]);
        pthread_mutex_lock(&h->lock);
        h->finished[k] = 1;
        h->running = -1;
        pthread_cond_broadcast(&h->done);
    }
    return NULL;
}

decode_helper_t* decode_helper_start(int core, int priority)
{
    if (s_started) return &s_helper;
    decode_helper_t* h = &s_helper;
    memset(h, 0, sizeof(*h));
    h->job = -1;
    h->running = -1;
    if (pthread_mutex_init(&h->lock, NULL) != 0) return NULL;
    if (pthread_cond_init(&h->wake, NULL) != 0 || pthread_cond_init(&h->done, NULL) != 0) return NULL;
#ifdef ESP_PLATFORM
    // LDPC alone puts about 6 KB on the stack (bp_decode()'s messages).
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192;
    cfg.prio = priority;
    cfg.pin_to_core = core;
    cfg.thread_name = "decode_helper";
    if (esp_pthread_set_cfg(&cfg) != 0) return NULL;
#else
    (void)core;
    (void)priority;
#endif
    if (pthread_create(&h->thread, NULL, helper_main, h) != 0) return NULL;
    s_started = 1;
    return h;
}

// Posts the first candidate after `i` that is neither sent nor skipped as things stand, if the helper is
// idle: one decode ahead, which two workers keep busy (docs/architecture/decode.md, Channels). Under the lock.
static void post_ahead(decode_helper_t* h, decode_core_t* core, int found, int i, uint8_t* sent)
{
    if (h->job >= 0 || h->running >= 0) return;
    for (int j = i + 1; j < found; ++j) {
        if (sent[j] || decode_core_skip(core, j)) continue;
        sent[j] = 1;
        h->job = j;
        pthread_cond_signal(&h->wake);
        return;
    }
}

int decode_drive_search(decode_core_t* core, int found, decode_helper_t* h, decode_new_fn on_new, void* context,
                        int* wasted)
{
    uint8_t sent[DECODE_MAX_CANDIDATES];
    uint8_t used[DECODE_MAX_CANDIDATES];
    if (found > DECODE_MAX_CANDIDATES) found = DECODE_MAX_CANDIDATES;
    memset(sent, 0, (size_t)found);
    memset(used, 0, (size_t)found);
    if (h) {
        pthread_mutex_lock(&h->lock);
        h->core = core;
        memset(h->finished, 0, (size_t)found);
        pthread_mutex_unlock(&h->lock);
    }
    int go = 1;
    for (int i = 0; i < found && go; ++i) {
        if (decode_core_skip(core, i)) continue;
        if (!decode_core_afford(core)) break;
        decode_core_attempt_t own;
        const decode_core_attempt_t* attempt = &own;
        if (h) {
            pthread_mutex_lock(&h->lock);
            post_ahead(h, core, found, i, sent);
            if (sent[i]) {
                while (!h->finished[i]) pthread_cond_wait(&h->done, &h->lock);
                attempt = &h->attempt[i];
            }
            pthread_mutex_unlock(&h->lock);
        }
        if (attempt == &own) decode_core_decode(core, i, &own);
        used[i] = 1;
        decode_core_message_t message;
        if (decode_core_commit(core, i, attempt, &message) == DECODE_CORE_NEW) go = on_new(context, &message);
    }
    if (h) {
        // The next search rewrites the candidates the helper reads: wait out the one it may hold.
        pthread_mutex_lock(&h->lock);
        h->job = -1;
        while (h->running >= 0) pthread_cond_wait(&h->done, &h->lock);
        pthread_mutex_unlock(&h->lock);
    }
    for (int k = 0; k < found; ++k) *wasted += sent[k] && !used[k];
    return go;
}
