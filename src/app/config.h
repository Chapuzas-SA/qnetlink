#ifndef QNETLINK_CONFIG_H
#define QNETLINK_CONFIG_H

#include <stdint.h>
#ifndef _WIN32
#include <netinet/in.h>
#else
#include <winsock2.h>
#endif

typedef enum TransportKind {
    TRANSPORT_TCP = 0,
    TRANSPORT_WSS = 1
} TransportKind;

typedef enum AppMode {
    APP_MODE_CLIENT = 0,
    APP_MODE_SERVER = 1
} AppMode;

typedef struct AppConfig {
    AppMode mode;
    int mode_explicit;
    char *net_cidr;
    char *dhcp_start_text;
    char *stream_host;
    char *listen_host;
    char *tls_cert_path;
    char *tls_key_path;
    TransportKind transport;
    char *ws_path;
    uint16_t stream_port;
    uint16_t listen_port;
    uint32_t reconnect_ms;
    int dhcp_start_explicit;
    int tls_insecure;
    int verbose;

    struct in_addr vnetwork;
    struct in_addr vnetmask;
    struct in_addr vhost;
    struct in_addr vdhcp_start;
    struct in_addr vnameserver;
} AppConfig;

void config_init(AppConfig *config);
void config_cleanup(AppConfig *config);
int config_parse(AppConfig *config, int *argc, char ***argv, int *show_version);
int config_validate(AppConfig *config);
int config_validate_server(const AppConfig *config);

#endif
