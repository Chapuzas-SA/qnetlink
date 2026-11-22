#include "event_loop.h"
#include "logger.h"
#include "net/platform.h"
#include "slirp_backend.h"
#include "net/transport.h"
#include <limits.h>
#include <stdlib.h>

typedef struct EventContext { SlirpBackend *slirp; Transport *transport; } EventContext;
static slirp_ssize_t slirp_output(const void *buf,size_t len,void *opaque) {
    EventContext *ctx=opaque; return (slirp_ssize_t)transport_send_ethernet(ctx->transport,buf,len);
}
static void transport_input(const uint8_t *packet,size_t len,void *opaque) {
    EventContext *ctx=opaque; slirp_backend_input(ctx->slirp,packet,len);
}
static uint32_t min_u32(uint32_t a,uint32_t b) { return a<b?a:b; }

int event_loop_run(const AppConfig *config) {
    EventContext ctx={0}; PlatformPollFd *fds=NULL; size_t cap=0; int exit_code=1;
    if (transport_init(&ctx.transport,config,transport_input,&ctx)!=0) { logger_log(LOG_LEVEL_ERROR,"app","transport_init_failed","failed to initialize transport"); return 1; }
    if (slirp_backend_init(&ctx.slirp,config,slirp_output,&ctx)!=0) { logger_log(LOG_LEVEL_ERROR,"slirp","init_failed","failed to initialize libslirp"); transport_cleanup(ctx.transport); return 1; }
    for (;;) {
        size_t count=0; uint32_t timeout=UINT32_MAX,t; int rc,poll_error=0;
        if (platform_stop_requested()) { exit_code=0; break; }
        if (transport_fill_poll(ctx.transport,&timeout,&fds,&count,&cap)!=0) { logger_log(LOG_LEVEL_ERROR,"app","poll_set_failed","out of memory while building transport poll set"); break; }
        t=slirp_backend_timer_timeout(ctx.slirp); timeout=min_u32(timeout,t);
        if (slirp_backend_fill_poll(ctx.slirp,&timeout,&fds,&count,&cap)!=0) { logger_log(LOG_LEVEL_ERROR,"app","poll_set_failed","out of memory while building libslirp poll set"); break; }
        t = 250U;
        if (timeout==UINT32_MAX || timeout>t) timeout=t;
        rc=platform_poll(fds,count,timeout>INT_MAX?INT_MAX:(int)timeout);
        if (rc<0) { if (platform_stop_requested()) { exit_code=0; break; } if (platform_poll_interrupted()) continue; poll_error=1; logger_log(LOG_LEVEL_ERROR,"app","poll_failed","poll failed"); }
        if (!poll_error && transport_poll(ctx.transport,fds)!=0) { logger_log(LOG_LEVEL_ERROR,"transport","service_failed","transport event dispatch failed"); break; }
        slirp_backend_poll(ctx.slirp,poll_error,fds); if (poll_error) break; slirp_backend_run_timers(ctx.slirp);
    }
    transport_cleanup(ctx.transport); slirp_backend_cleanup(ctx.slirp); free(fds); return exit_code;
}
