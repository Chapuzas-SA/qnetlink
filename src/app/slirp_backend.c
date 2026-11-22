#include "slirp_backend.h"
#include "logger.h"
#include <stdio.h>
#ifndef _WIN32
#include <arpa/inet.h>
#else
#include <ws2tcpip.h>
#endif
#include <stdlib.h>
#include <string.h>

typedef struct BackendTimer {
    SlirpTimerId id; void *cb_opaque; int active; int64_t expires_ms;
    struct BackendTimer *next;
} BackendTimer;

struct SlirpBackend {
    Slirp *slirp; SlirpBackendOutput output; void *output_opaque;
    BackendTimer *timers;
    PlatformPollFd **poll_fds_ptr; size_t *poll_capacity;
    size_t poll_base; size_t poll_count; int poll_add_failed;
};

static slirp_ssize_t cb_send(const void *buf, size_t len, void *opaque)
{
    SlirpBackend *b = opaque;
    return b->output ? b->output(buf, len, b->output_opaque) : (slirp_ssize_t)len;
}
static void cb_guest_error(const char *msg, void *opaque) { (void)opaque; logger_log(LOG_LEVEL_WARN, "slirp", "guest_error", "%s", msg); }
static int64_t cb_clock_ns(void *opaque) { (void)opaque; return platform_clock_ns(); }
static void cb_init_completed(Slirp *s, void *opaque) { ((SlirpBackend *)opaque)->slirp = s; }
static void *cb_timer_new(SlirpTimerId id, void *cb_opaque, void *opaque)
{
    SlirpBackend *b = opaque; BackendTimer *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->id = id; t->cb_opaque = cb_opaque; t->next = b->timers; b->timers = t; return t;
}
static void cb_timer_free(void *timer, void *opaque)
{
    SlirpBackend *b = opaque; BackendTimer **pp = &b->timers;
    while (*pp && *pp != timer) pp = &(*pp)->next;
    if (*pp) { BackendTimer *t = *pp; *pp = t->next; free(t); }
}
static void cb_timer_mod(void *timer, int64_t expires, void *opaque)
{ (void)opaque; ((BackendTimer *)timer)->expires_ms = expires; ((BackendTimer *)timer)->active = 1; }
static void cb_register(slirp_os_socket fd, void *opaque) { (void)fd; (void)opaque; }
static void cb_unregister(slirp_os_socket fd, void *opaque) { (void)fd; (void)opaque; }
static void cb_notify(void *opaque) { (void)opaque; }

static const SlirpCb callbacks = {
    .send_packet = cb_send, .guest_error = cb_guest_error, .clock_get_ns = cb_clock_ns,
    .timer_free = cb_timer_free, .timer_mod = cb_timer_mod, .notify = cb_notify,
    .init_completed = cb_init_completed, .timer_new_opaque = cb_timer_new,
    .register_poll_socket = cb_register, .unregister_poll_socket = cb_unregister,
};

int slirp_backend_init(SlirpBackend **out, const AppConfig *config,
                       SlirpBackendOutput output, void *opaque)
{
    SlirpBackend *b = calloc(1, sizeof(*b)); SlirpConfig cfg;
    if (!b) return -1;
    b->output = output; b->output_opaque = opaque;
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 6; cfg.restricted = 0; cfg.in_enabled = true; cfg.in6_enabled = false;
    cfg.vnetwork = config->vnetwork;
    cfg.vnetmask = config->vnetmask;
    cfg.vhost = config->vhost;
    cfg.vdhcp_start = config->vdhcp_start;
    cfg.vnameserver = config->vnameserver;
    b->slirp = slirp_new(&cfg, &callbacks, b);
    if (!b->slirp) { free(b); return -1; }
    *out = b; return 0;
}
void slirp_backend_cleanup(SlirpBackend *b)
{
    BackendTimer *t, *next; if (!b) return;
    if (b->slirp) slirp_cleanup(b->slirp);
    for (t = b->timers; t; t = next) { next = t->next; free(t); }
    free(b);
}
void slirp_backend_input(SlirpBackend *b, const uint8_t *p, size_t len)
{ if (b && b->slirp && p && len <= 0x7fffffffU) slirp_input(b->slirp, p, (int)len); }

static int add_poll(slirp_os_socket fd, int events, void *opaque)
{
    SlirpBackend *b = opaque;
    size_t pos = b->poll_base + b->poll_count;
    if (pos >= *b->poll_capacity) {
        size_t new_cap = *b->poll_capacity ? *b->poll_capacity * 2U : 16U;
        PlatformPollFd *n;
        if (new_cap <= pos) new_cap = pos + 1U;
        if (new_cap > SIZE_MAX / sizeof(**b->poll_fds_ptr)) {
            b->poll_add_failed = 1;
            return -1;
        }
        n = realloc(*b->poll_fds_ptr, new_cap * sizeof(**b->poll_fds_ptr));
        if (!n) {
            b->poll_add_failed = 1;
            return -1;
        }
        *b->poll_fds_ptr = n;
        *b->poll_capacity = new_cap;
    }
    {
        PlatformPollFd *p = &(*b->poll_fds_ptr)[pos];
        p->fd = fd; p->events = 0; p->revents = 0;
        if (events & (SLIRP_POLL_IN | SLIRP_POLL_PRI)) p->events |= PLATFORM_POLL_IN;
        if (events & SLIRP_POLL_OUT) p->events |= PLATFORM_POLL_OUT;
    }
    b->poll_count++;
    return (int)(b->poll_count - 1U);
}
int slirp_backend_fill_poll(SlirpBackend *b, uint32_t *timeout,
                            PlatformPollFd **fds, size_t *count, size_t *capacity)
{
    b->poll_fds_ptr = fds;
    b->poll_capacity = capacity;
    b->poll_base = *count;
    b->poll_count = 0;
    b->poll_add_failed = 0;
    slirp_pollfds_fill_socket(b->slirp, timeout, add_poll, b);
    if (b->poll_add_failed) return -1;
    *count += b->poll_count;
    return 0;
}
static int get_revents(int idx, void *opaque)
{
    SlirpBackend *b = opaque; int r = (*b->poll_fds_ptr)[b->poll_base + (size_t)idx].revents, out = 0;
    if (r & PLATFORM_POLL_IN) out |= SLIRP_POLL_IN;
    if (r & PLATFORM_POLL_OUT) out |= SLIRP_POLL_OUT;
    if (r & PLATFORM_POLL_ERR) out |= SLIRP_POLL_ERR;
    if (r & PLATFORM_POLL_HUP) out |= SLIRP_POLL_HUP;
    return out;
}
void slirp_backend_poll(SlirpBackend *b, int err, PlatformPollFd *fds)
{ b->poll_fds_ptr = &fds; slirp_pollfds_poll(b->slirp, err, get_revents, b); }
uint32_t slirp_backend_timer_timeout(SlirpBackend *b)
{
    BackendTimer *t; int64_t now = platform_clock_ms(), best = INT64_MAX;
    for (t = b->timers; t; t = t->next) if (t->active && t->expires_ms < best) best = t->expires_ms;
    if (best == INT64_MAX) return UINT32_MAX;
    if (best <= now) return 0;
    return (uint64_t)(best - now) > UINT32_MAX ? UINT32_MAX : (uint32_t)(best - now);
}
void slirp_backend_run_timers(SlirpBackend *b)
{
    BackendTimer *t; int64_t now = platform_clock_ms();
    for (t = b->timers; t; t = t->next) if (t->active && t->expires_ms <= now) {
        t->active = 0; slirp_handle_timer(b->slirp, t->id, t->cb_opaque);
    }
}
