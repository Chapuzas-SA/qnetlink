#ifndef QNETLINK_SLIRP_BACKEND_H
#define QNETLINK_SLIRP_BACKEND_H
#include "net/platform.h"
#include "app/config.h"
#include <stddef.h>
#include <stdint.h>
typedef slirp_ssize_t (*SlirpBackendOutput)(const void *, size_t, void *);
typedef struct SlirpBackend SlirpBackend;
int slirp_backend_init(SlirpBackend **out, const AppConfig *config,
                       SlirpBackendOutput output, void *opaque);
void slirp_backend_cleanup(SlirpBackend *b);
void slirp_backend_input(SlirpBackend *b, const uint8_t *packet, size_t len);
int slirp_backend_fill_poll(SlirpBackend *b, uint32_t *timeout,
                            PlatformPollFd **fds, size_t *count, size_t *capacity);
void slirp_backend_poll(SlirpBackend *b, int poll_error, PlatformPollFd *fds);
void slirp_backend_run_timers(SlirpBackend *b);
uint32_t slirp_backend_timer_timeout(SlirpBackend *b);
#endif
