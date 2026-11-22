#include "config.h"
#include "version.h"
#include "event_loop.h"
#include "net/platform.h"
#include "net/wss_server.h"
#include "logger.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    AppConfig config;
    int rc, show_version = 0;

    config_init(&config);
    if (config_parse(&config, &argc, &argv, &show_version) != 0) {
        config_cleanup(&config);
        return 2;
    }
    if (show_version < 0) {
        config_cleanup(&config);
        return 0;
    }
    logger_set_context(config.mode == APP_MODE_SERVER ? "server" : "client",
                       config.mode == APP_MODE_SERVER || config.transport == TRANSPORT_WSS ? "wss" : "tcp",
                       config.verbose);
    if (show_version) {
        printf("qnetlink %s build=%s\n", QNETLINK_VERSION, QNETLINK_BUILD_ID);
        config_cleanup(&config);
        return 0;
    }
    if (config.mode == APP_MODE_CLIENT && config_validate(&config) != 0) {
        config_cleanup(&config);
        return 2;
    }
    if (config.mode == APP_MODE_SERVER && config_validate_server(&config) != 0) {
        config_cleanup(&config);
        return 2;
    }
    if (platform_init() != 0) {
        logger_log(LOG_LEVEL_ERROR, "app", "platform_init_failed", "platform initialization failed");
        config_cleanup(&config);
        return 1;
    }

    if (config.mode == APP_MODE_SERVER) {
        rc = wss_server_run(&config);
    } else {
        logger_log(LOG_LEVEL_INFO, "app", "started", "peer=%s:%u network=%s dhcp_start=%s",
               config.stream_host, (unsigned)config.stream_port,
               config.net_cidr, config.dhcp_start_text);
        rc = event_loop_run(&config);
    }
    platform_cleanup();
    config_cleanup(&config);
    return rc;
}
