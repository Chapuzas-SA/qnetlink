#include "config.h"
#include "logger.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#else
#include <ws2tcpip.h>
#endif

#define DEFAULT_NET "10.255.254.0/24"
#define DEFAULT_DHCP_START "10.255.254.15"
#define DEFAULT_STREAM_HOST "127.0.0.1"
#define DEFAULT_LISTEN_HOST "0.0.0.0"
#define DEFAULT_STREAM_PORT 9090
#define DEFAULT_LISTEN_PORT 443
#define DEFAULT_RECONNECT_MS 1000
#define DEFAULT_WS_PATH "/"

static int replace_string(char **dst, const char *src)
{
    char *copy = g_strdup(src);
    if (!copy) return -1;
    g_free(*dst);
    *dst = copy;
    return 0;
}

static gboolean config_key_allowed(const char *group, const char *key)
{
    static const struct { const char *group; const char *key; } allowed[] = {
        { "general", "mode" }, { "general", "verbose" }, { "general", "reconnect-ms" },
        { "stream", "host" }, { "stream", "port" },
        { "client", "net" }, { "client", "dhcp-start" }, { "client", "transport" },
        { "wss", "path" }, { "wss", "tls-insecure" },
        { "server", "listen-host" }, { "server", "listen-port" },
        { "server", "cert" }, { "server", "key" }
    };
    for (size_t i = 0; i < G_N_ELEMENTS(allowed); ++i) {
        if (strcmp(group, allowed[i].group) == 0 && strcmp(key, allowed[i].key) == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean keyfile_string(GKeyFile *file, const char *group, const char *key,
                               char **destination, GError **error)
{
    if (!g_key_file_has_key(file, group, key, NULL)) return TRUE;
    char *value = g_key_file_get_string(file, group, key, error);
    if (!value) return FALSE;
    g_free(*destination);
    *destination = value;
    return TRUE;
}

static gboolean keyfile_integer(GKeyFile *file, const char *group, const char *key,
                                gint64 *destination, GError **error)
{
    if (!g_key_file_has_key(file, group, key, NULL)) return TRUE;
    *destination = g_key_file_get_int64(file, group, key, error);
    return *error == NULL;
}

static gboolean keyfile_boolean(GKeyFile *file, const char *group, const char *key,
                                gboolean *destination, GError **error)
{
    if (!g_key_file_has_key(file, group, key, NULL)) return TRUE;
    *destination = g_key_file_get_boolean(file, group, key, error);
    return *error == NULL;
}

static int config_load_file(AppConfig *config, const char *path)
{
    GKeyFile *file = g_key_file_new();
    GError *error = NULL;
    gboolean verbose = config->verbose != 0;
    gboolean tls_insecure = config->tls_insecure != 0;
    gint64 reconnect_ms = config->reconnect_ms;
    gint64 stream_port = config->stream_port;
    gint64 listen_port = config->listen_port;
    char **groups = NULL;
    gsize group_count = 0;
    int rc = -1;

    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error)) goto fail;
    groups = g_key_file_get_groups(file, &group_count);
    for (gsize i = 0; i < group_count; ++i) {
        gsize key_count = 0;
        char **keys = g_key_file_get_keys(file, groups[i], &key_count, &error);
        if (!keys) goto fail;
        for (gsize j = 0; j < key_count; ++j) {
            if (!config_key_allowed(groups[i], keys[j])) {
                g_set_error(&error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                            "unknown configuration key '%s' in section [%s]", keys[j], groups[i]);
                g_strfreev(keys);
                goto fail;
            }
        }
        g_strfreev(keys);
    }

#define GET_STRING(group, key, field) \
    if (!keyfile_string(file, group, key, &config->field, &error)) goto fail
#define GET_INT(group, key, variable) \
    if (!keyfile_integer(file, group, key, &variable, &error)) goto fail
#define GET_BOOL(group, key, variable) \
    if (!keyfile_boolean(file, group, key, &variable, &error)) goto fail
    GET_STRING("stream", "host", stream_host);
    GET_STRING("server", "listen-host", listen_host);
    GET_STRING("server", "cert", tls_cert_path);
    GET_STRING("server", "key", tls_key_path);
    GET_STRING("client", "net", net_cidr);
    if (g_key_file_has_key(file, "client", "dhcp-start", NULL)) {
        GET_STRING("client", "dhcp-start", dhcp_start_text);
        config->dhcp_start_explicit = 1;
    }
    GET_STRING("wss", "path", ws_path);
    GET_INT("general", "reconnect-ms", reconnect_ms);
    GET_INT("stream", "port", stream_port);
    GET_INT("server", "listen-port", listen_port);
    GET_BOOL("general", "verbose", verbose);
    GET_BOOL("wss", "tls-insecure", tls_insecure);
#undef GET_STRING
#undef GET_INT
#undef GET_BOOL

    if (g_key_file_has_key(file, "general", "mode", NULL)) {
        char *value = g_key_file_get_string(file, "general", "mode", &error);
        if (!value) goto fail;
        if (strcmp(value, "client") == 0) config->mode = APP_MODE_CLIENT;
        else if (strcmp(value, "server") == 0) config->mode = APP_MODE_SERVER;
        else {
            g_set_error(&error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                        "invalid value '%s' for [general] mode (expected client or server)", value);
            g_free(value);
            goto fail;
        }
        config->mode_explicit = 1;
        g_free(value);
    }
    if (g_key_file_has_key(file, "client", "transport", NULL)) {
        char *value = g_key_file_get_string(file, "client", "transport", &error);
        if (!value) goto fail;
        if (strcmp(value, "tcp") == 0) config->transport = TRANSPORT_TCP;
        else if (strcmp(value, "wss") == 0) config->transport = TRANSPORT_WSS;
        else {
            g_set_error(&error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                        "invalid value '%s' for [client] transport (expected tcp or wss)", value);
            g_free(value);
            goto fail;
        }
        g_free(value);
    }
    if (stream_port < 1 || stream_port > 65535 || listen_port < 1 || listen_port > 65535 ||
        reconnect_ms < 0 || reconnect_ms > UINT32_MAX) {
        g_set_error(&error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                    "port values must be 1..65535 and reconnect-ms must be 0..%u", UINT32_MAX);
        goto fail;
    }
    config->stream_port = (uint16_t)stream_port;
    config->listen_port = (uint16_t)listen_port;
    config->reconnect_ms = (uint32_t)reconnect_ms;
    config->verbose = verbose ? 1 : 0;
    config->tls_insecure = tls_insecure ? 1 : 0;
    rc = 0;
    goto out;

fail:
    logger_log(LOG_LEVEL_ERROR, "config", "config_file_error", "cannot read config file '%s': %s",
               path, error ? error->message : "invalid configuration");
out:
    if (error) g_error_free(error);
    g_strfreev(groups);
    g_key_file_unref(file);
    return rc;
}

void config_init(AppConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->mode = APP_MODE_CLIENT;
    config->net_cidr = g_strdup(DEFAULT_NET);
    config->dhcp_start_text = g_strdup(DEFAULT_DHCP_START);
    config->stream_host = g_strdup(DEFAULT_STREAM_HOST);
    config->listen_host = g_strdup(DEFAULT_LISTEN_HOST);
    config->transport = TRANSPORT_TCP;
    config->ws_path = g_strdup(DEFAULT_WS_PATH);
    config->stream_port = DEFAULT_STREAM_PORT;
    config->listen_port = DEFAULT_LISTEN_PORT;
    config->reconnect_ms = DEFAULT_RECONNECT_MS;
}

void config_cleanup(AppConfig *config)
{
    if (!config) return;
    g_free(config->net_cidr);
    g_free(config->dhcp_start_text);
    g_free(config->stream_host);
    g_free(config->listen_host);
    g_free(config->tls_cert_path);
    g_free(config->tls_key_path);
    g_free(config->ws_path);
    memset(config, 0, sizeof(*config));
}

int config_parse(AppConfig *config, int *argc, char ***argv, int *show_version)
{
    gchar *mode = NULL, *net = NULL, *dhcp_start = NULL, *stream_host = NULL;
    gchar *listen_host = NULL, *transport = NULL, *ws_path = NULL;
    gchar *tls_cert_path = NULL, *tls_key_path = NULL;
    gchar *config_path = NULL;
    gchar *config_file = NULL;
    gint stream_port;
    gint listen_port;
    gint64 reconnect_ms;
    gboolean version = FALSE;
    gboolean tls_insecure;
    gboolean verbose;
    gboolean no_tls_insecure = FALSE;
    gboolean no_verbose = FALSE;
    GError *error = NULL;
    GOptionContext *context;
    GOptionGroup *group;
    GOptionGroup *help_groups[5];
    const char *help_titles[] = {
        "General options", "Stream options (both roles)", "Client mode options",
        "WSS / TLS options", "Server mode options"
    };
    GOptionEntry general_entries[] = {
        { "config", 0, 0, G_OPTION_ARG_FILENAME, &config_path, "Read options from an INI configuration file", "FILE" },
        { "mode", 0, 0, G_OPTION_ARG_STRING, &mode, "Required runtime role: client or server", "MODE" },
        { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Enable debug logging", NULL },
        { "no-verbose", 0, 0, G_OPTION_ARG_NONE, &no_verbose, "Disable debug logging, overriding the config file", NULL },
        { "reconnect-ms", 0, 0, G_OPTION_ARG_INT64, &reconnect_ms, "Retry delay in either role (default: 1000)", "MILLISECONDS" },
        { "version", 'V', 0, G_OPTION_ARG_NONE, &version, "Show version information and exit", NULL },
        { NULL }
    };
    GOptionEntry stream_entries[] = {
        { "stream-host", 0, 0, G_OPTION_ARG_STRING, &stream_host, "Stream peer host (default: 127.0.0.1)", "HOST" },
        { "stream-port", 0, 0, G_OPTION_ARG_INT, &stream_port, "Stream peer port (default: 9090)", "PORT" },
        { NULL }
    };
    GOptionEntry client_entries[] = {
        { "net", 0, 0, G_OPTION_ARG_STRING, &net, "Client only: guest IPv4 network (default: 10.255.254.0/24)", "NETWORK" },
        { "dhcp-start", 0, 0, G_OPTION_ARG_STRING, &dhcp_start, "Client only: DHCP start address (default: derived as QEMU .15)", "ADDRESS" },
        { "transport", 0, 0, G_OPTION_ARG_STRING, &transport, "Client only: TCP or WSS (default: tcp); server always uses WSS", "TRANSPORT" },
        { NULL }
    };
    GOptionEntry wss_entries[] = {
        { "ws-path", 0, 0, G_OPTION_ARG_STRING, &ws_path, "Client WSS and server: WebSocket path (default: /)", "PATH" },
        { "tls-insecure", 0, 0, G_OPTION_ARG_NONE, &tls_insecure, "Client WSS only: accept self-signed certificates and skip hostname checks (testing only)", NULL },
        { "tls-verify", 0, 0, G_OPTION_ARG_NONE, &no_tls_insecure, "Client WSS only: enable certificate verification, overriding the config file", NULL },
        { NULL }
    };
    GOptionEntry server_entries[] = {
        { "listen-port", 0, 0, G_OPTION_ARG_INT, &listen_port, "Server only: WSS listen port (default: 443)", "PORT" },
        { "cert", 0, 0, G_OPTION_ARG_STRING, &tls_cert_path, "Server only: TLS certificate file", "FILE" },
        { "key", 0, 0, G_OPTION_ARG_STRING, &tls_key_path, "Server only: TLS private key file", "FILE" },
        { "listen-host", 0, 0, G_OPTION_ARG_STRING, &listen_host, "Server only: local IPv4 address to bind (default: 0.0.0.0)", "IPv4" },
        { NULL }
    };
    int rc = -1;

    if (!config->net_cidr || !config->dhcp_start_text || !config->stream_host || !config->listen_host || !config->ws_path) {
        logger_log(LOG_LEVEL_ERROR, "config", "out_of_memory", "unable to initialize defaults");
        return -1;
    }

    context = g_option_context_new(NULL);
    if (!context) {
        logger_log(LOG_LEVEL_ERROR, "config", "out_of_memory", "unable to create argument parser");
        return -1;
    }
    g_option_context_set_summary(context, "QEMU stream-based VM TCP bridge.");
    group = g_option_group_new("general", "General options", "General options", NULL, NULL);
    help_groups[0] = group;
    g_option_group_add_entries(group, general_entries);
    g_option_context_add_group(context, group);
    group = g_option_group_new("stream", "Stream options (both roles)", "Stream options", NULL, NULL);
    help_groups[1] = group;
    g_option_group_add_entries(group, stream_entries);
    g_option_context_add_group(context, group);
    group = g_option_group_new("client", "Client mode options", "Client mode options", NULL, NULL);
    help_groups[2] = group;
    g_option_group_add_entries(group, client_entries);
    g_option_context_add_group(context, group);
    group = g_option_group_new("wss", "WSS / TLS options", "WSS / TLS options", NULL, NULL);
    help_groups[3] = group;
    g_option_group_add_entries(group, wss_entries);
    g_option_context_add_group(context, group);
    group = g_option_group_new("server", "Server mode options", "Server mode options", NULL, NULL);
    help_groups[4] = group;
    g_option_group_add_entries(group, server_entries);
    g_option_context_add_group(context, group);

    gboolean show_help = *argc == 1;
    for (int i = 1; i < *argc; ++i) {
        if (strcmp((*argv)[i], "-h") == 0 || strcmp((*argv)[i], "--help") == 0 ||
            strcmp((*argv)[i], "--help-all") == 0) {
            show_help = TRUE;
            break;
        }
    }
    if (show_help) {
        fputs("Usage: qnetlink [OPTION?]\n\nQEMU stream-based VM TCP bridge.\n", stdout);
        for (size_t n = 0; n < G_N_ELEMENTS(help_groups); ++n) {
            gchar *help = g_option_context_get_help(context, FALSE, help_groups[n]);
            const char *section = strstr(help, help_titles[n]);
            if (section) {
                fputc('\n', stdout);
                fputs(section, stdout);
            }
            g_free(help);
        }
        *show_version = -1;
        rc = 0;
        goto out;
    }

    for (int i = 1; i < *argc; ++i) {
        if (strcmp((*argv)[i], "--config") == 0) {
            if (i + 1 >= *argc || config_file) {
                logger_log(LOG_LEVEL_ERROR, "config", "invalid_config_option", "--config requires exactly one file path");
                goto out;
            }
            config_file = g_strdup((*argv)[i + 1]);
            if (!config_file) goto oom;
            i++;
        } else if (g_str_has_prefix((*argv)[i], "--config=")) {
            if (config_file || !(*argv)[i][9]) {
                logger_log(LOG_LEVEL_ERROR, "config", "invalid_config_option", "--config requires exactly one file path");
                goto out;
            }
            config_file = g_strdup((*argv)[i] + 9);
            if (!config_file) goto oom;
        }
    }
#ifdef _WIN32
    if (config_file) {
        gchar *native_path = g_filename_from_utf8(config_file, -1, NULL, NULL, &error);
        if (!native_path) {
            logger_log(LOG_LEVEL_ERROR, "config", "invalid_config_path", "invalid filename encoding for --config: %s",
                       error ? error->message : "conversion failed");
            if (error) {
                g_error_free(error);
                error = NULL;
            }
            goto out;
        }
        g_free(config_file);
        config_file = native_path;
    }
#endif
    if (config_file && config_load_file(config, config_file) != 0) goto out;
    stream_port = config->stream_port;
    listen_port = config->listen_port;
    reconnect_ms = config->reconnect_ms;
    tls_insecure = config->tls_insecure != 0;
    verbose = config->verbose != 0;

    if (!g_option_context_parse(context, argc, argv, &error)) {
        logger_log(LOG_LEVEL_ERROR, "config", "invalid_arguments", "%s; try --help for more information", error->message);
        g_error_free(error);
        goto out;
    }
    if (*argc != 1) {
        logger_log(LOG_LEVEL_ERROR, "config", "unexpected_argument", "unexpected argument: '%s'; try --help for more information", (*argv)[1]);
        goto out;
    }
    if (version) {
        *show_version = 1;
        rc = 0;
        goto out;
    }
    if (stream_port < 1 || stream_port > 65535) {
        logger_log(LOG_LEVEL_ERROR, "config", "invalid_stream_port", "invalid value for --stream-port: %d", stream_port);
        goto out;
    }
    if (listen_port < 1 || listen_port > 65535) {
        logger_log(LOG_LEVEL_ERROR, "config", "invalid_listen_port", "invalid value for --listen-port: %d", listen_port);
        goto out;
    }
    if (reconnect_ms < 0 || reconnect_ms > UINT32_MAX) {
        logger_log(LOG_LEVEL_ERROR, "config", "invalid_reconnect_delay", "invalid value for --reconnect-ms: %" G_GINT64_FORMAT, reconnect_ms);
        goto out;
    }
    if (mode) {
        if (strcmp(mode, "client") == 0) config->mode = APP_MODE_CLIENT;
        else if (strcmp(mode, "server") == 0) config->mode = APP_MODE_SERVER;
        else {
            logger_log(LOG_LEVEL_ERROR, "config", "invalid_mode", "invalid --mode '%s' (expected client or server)", mode);
            goto out;
        }
        config->mode_explicit = 1;
    }
    if (!config->mode_explicit) {
        logger_log(LOG_LEVEL_ERROR, "config", "missing_mode",
                   "runtime role is required; specify --mode=client or --mode=server, or set mode in the config file");
        goto out;
    }
    if (net && replace_string(&config->net_cidr, net) != 0) goto oom;
    if (dhcp_start) {
        if (replace_string(&config->dhcp_start_text, dhcp_start) != 0) goto oom;
        config->dhcp_start_explicit = 1;
    }
    if (stream_host && replace_string(&config->stream_host, stream_host) != 0) goto oom;
    if (listen_host && replace_string(&config->listen_host, listen_host) != 0) goto oom;
    if (tls_cert_path && replace_string(&config->tls_cert_path, tls_cert_path) != 0) goto oom;
    if (tls_key_path && replace_string(&config->tls_key_path, tls_key_path) != 0) goto oom;
    config->tls_insecure = no_tls_insecure ? 0 : (tls_insecure ? 1 : 0);
    config->verbose = no_verbose ? 0 : (verbose ? 1 : 0);
    if (transport) {
        if (strcmp(transport, "tcp") == 0) config->transport = TRANSPORT_TCP;
        else if (strcmp(transport, "wss") == 0) config->transport = TRANSPORT_WSS;
        else {
            logger_log(LOG_LEVEL_ERROR, "config", "invalid_transport", "invalid --transport '%s' (expected tcp or wss)", transport);
            goto out;
        }
    }
    if (ws_path && replace_string(&config->ws_path, ws_path) != 0) goto oom;
    if (!config->ws_path[0] || config->ws_path[0] != '/') {
        logger_log(LOG_LEVEL_ERROR, "config", "invalid_ws_path", "--ws-path must begin with '/'");
        goto out;
    }
    {
        const unsigned char *p = (const unsigned char *)config->ws_path;
        for (; *p; ++p) {
            if (*p < 0x20U || *p == 0x7fU) {
                logger_log(LOG_LEVEL_ERROR, "config", "invalid_ws_path", "--ws-path must not contain control characters");
                goto out;
            }
        }
    }
    if (!config->stream_host[0]) {
        logger_log(LOG_LEVEL_ERROR, "config", "empty_stream_host", "--stream-host must not be empty");
        goto out;
    }
    if (!config->listen_host[0]) {
        logger_log(LOG_LEVEL_ERROR, "config", "empty_listen_host", "--listen-host must not be empty");
        goto out;
    }
    config->stream_port = (uint16_t)stream_port;
    config->listen_port = (uint16_t)listen_port;
    config->reconnect_ms = (uint32_t)reconnect_ms;
    *show_version = version ? 1 : 0;
    rc = 0;
    goto out;

oom:
    logger_log(LOG_LEVEL_ERROR, "config", "out_of_memory", "unable to store command-line option");
out:
    g_free(config_file);
    g_free(mode);
    g_free(net);
    g_free(dhcp_start);
    g_free(stream_host);
    g_free(listen_host);
    g_free(transport);
    g_free(ws_path);
    g_free(tls_cert_path);
    g_free(tls_key_path);
    g_free(config_path);
    g_option_context_free(context);
    return rc;
}

static int parse_cidr(const char *text, struct in_addr *network, struct in_addr *mask)
{
    char addr[INET_ADDRSTRLEN];
    const char *slash = strchr(text, '/');
    char *end = NULL;
    unsigned long prefix;
    uint32_t host_mask, host_addr;
    size_t len;

    if (!slash || strchr(slash + 1, '/')) return -1;
    len = (size_t)(slash - text);
    if (len == 0 || len >= sizeof(addr)) return -1;
    memcpy(addr, text, len); addr[len] = '\0';
    prefix = strtoul(slash + 1, &end, 10);
    if (!slash[1] || !end || *end || prefix > 32) return -1;
    if (inet_pton(AF_INET, addr, network) != 1) return -1;

    host_mask = prefix == 0 ? 0U : UINT32_MAX << (32U - (unsigned)prefix);
    host_addr = ntohl(network->s_addr);
    if ((host_addr & ~host_mask) != 0) return -2;
    network->s_addr = htonl(host_addr & host_mask);
    mask->s_addr = htonl(host_mask);
    return 0;
}

static struct in_addr derive_address(struct in_addr network, struct in_addr mask, uint32_t qemu_offset)
{
    struct in_addr out;
    uint32_t n = ntohl(network.s_addr), m = ntohl(mask.s_addr);
    out.s_addr = htonl(n | (qemu_offset & ~m));
    return out;
}

static int same_addr(struct in_addr a, struct in_addr b) { return a.s_addr == b.s_addr; }
static int in_network(struct in_addr a, struct in_addr n, struct in_addr m)
{ return (a.s_addr & m.s_addr) == n.s_addr; }

int config_validate(AppConfig *config)
{
    int rc = parse_cidr(config->net_cidr, &config->vnetwork, &config->vnetmask);
    if (rc != 0) {
        if (rc == -2)
            logger_log(LOG_LEVEL_ERROR, "config", "invalid_network", "--net must specify the network address: '%s'", config->net_cidr);
        else
            logger_log(LOG_LEVEL_ERROR, "config", "invalid_network", "invalid IPv4 CIDR for --net: '%s'", config->net_cidr);
        return -1;
    }

    /* Match QEMU user-net defaults relative to the selected network. */
    config->vhost = derive_address(config->vnetwork, config->vnetmask, 0x0202U);
    config->vnameserver = derive_address(config->vnetwork, config->vnetmask, 0x0203U);

    if (!config->dhcp_start_explicit) {
        char text[INET_ADDRSTRLEN];
        config->vdhcp_start = derive_address(config->vnetwork, config->vnetmask, 0x020fU);
        if (!inet_ntop(AF_INET, &config->vdhcp_start, text, sizeof(text)) ||
            replace_string(&config->dhcp_start_text, text) != 0) {
            logger_log(LOG_LEVEL_ERROR, "config", "dhcp_address_failed", "failed to derive DHCP address");
            return -1;
        }
    } else if (inet_pton(AF_INET, config->dhcp_start_text, &config->vdhcp_start) != 1) {
        logger_log(LOG_LEVEL_ERROR, "config", "invalid_dhcp_address", "invalid IPv4 address for --dhcp-start: '%s'", config->dhcp_start_text);
        return -1;
    }
    if (!in_network(config->vdhcp_start, config->vnetwork, config->vnetmask)) {
        logger_log(LOG_LEVEL_ERROR, "config", "dhcp_outside_network", "--dhcp-start is outside --net");
        return -1;
    }
    if (same_addr(config->vdhcp_start, config->vhost) ||
        same_addr(config->vdhcp_start, config->vnameserver)) {
        logger_log(LOG_LEVEL_ERROR, "config", "dhcp_address_conflict", "--dhcp-start conflicts with the virtual host or DNS address");
        return -1;
    }
    return 0;
}

int config_validate_server(const AppConfig *config)
{
    struct in_addr address;

    if (!config->tls_cert_path || !config->tls_cert_path[0]) {
        logger_log(LOG_LEVEL_ERROR, "config", "missing_certificate", "server mode requires --cert");
        return -1;
    }
    if (!config->tls_key_path || !config->tls_key_path[0]) {
        logger_log(LOG_LEVEL_ERROR, "config", "missing_private_key", "server mode requires --key");
        return -1;
    }
    if (inet_pton(AF_INET, config->listen_host, &address) != 1) {
        logger_log(LOG_LEVEL_ERROR, "config", "invalid_listen_host",
                   "--listen-host must be an IPv4 address (for example 0.0.0.0 or 127.0.0.1)");
        return -1;
    }
    return 0;
}
