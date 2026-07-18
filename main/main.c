#include "board.h"
#include "config.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define APP_LOG_QUEUE_LENGTH          16U
#define APP_RAW_QUEUE_LENGTH          4U
#define APP_SAMPLE_QUEUE_LENGTH       16U

#define APP_TASK_STACK_MAIN           4096U
#define APP_TASK_STACK_NETWORK        3072U
#define APP_TASK_STACK_ETHERNET       3072U
#define APP_TASK_STACK_MODBUS         4096U
#define APP_TASK_STACK_PROCESSING     4096U
#define APP_TASK_STACK_HTTP           16384U
#define APP_TASK_STACK_DIAGNOSTICS    4096U
#define APP_TASK_STACK_LOGGER         4096U
#define APP_TASK_STACK_WATCHDOG       3072U
#define APP_TASK_STACK_LED            2048U

#define APP_PRIORITY_NETWORK          6U
#define APP_PRIORITY_ETHERNET         6U
#define APP_PRIORITY_MODBUS           5U
#define APP_PRIORITY_PROCESSING       5U
#define APP_PRIORITY_HTTP             4U
#define APP_PRIORITY_DIAGNOSTICS      2U
#define APP_PRIORITY_LOGGER           3U
#define APP_PRIORITY_WATCHDOG         3U
#define APP_PRIORITY_LED              1U

#define APP_EVENT_NETWORK_READY       (1U << 0)
#define APP_EVENT_ETHERNET_LINK_UP    (1U << 1)
#define APP_EVENT_MODBUS_HEALTHY      (1U << 2)
#define APP_EVENT_HTTP_HEALTHY        (1U << 3)
#define APP_EVENT_PROCESSING_HEALTHY  (1U << 4)
#define APP_EVENT_DIAGNOSTICS_HEALTHY (1U << 5)
#define APP_EVENT_LED_HEALTHY         (1U << 6)

#define HTTP_UPLOAD_PERIOD_MS         3600000U
#define HTTP_CONNECT_TIMEOUT_MS       5000

#define WEB_CONFIG_POST_MAX_BYTES     255U
#define NVS_NAMESPACE_NETWORK         "net_cfg"
#define NVS_KEY_IP                    "ip"
#define NVS_KEY_GATEWAY               "gw"
#define NVS_KEY_NETMASK               "mask"
#define NVS_KEY_DNS                   "dns"
#define NVS_KEY_BACKUP_DNS            "bdns"

#define WEB_CONFIG_PORTAL_IP_A        192
#define WEB_CONFIG_PORTAL_IP_B        168
#define WEB_CONFIG_PORTAL_IP_C        4
#define WEB_CONFIG_PORTAL_IP_D        1

#define WEB_CONFIG_PORTAL_NETMASK_A   255
#define WEB_CONFIG_PORTAL_NETMASK_B   255
#define WEB_CONFIG_PORTAL_NETMASK_C   255
#define WEB_CONFIG_PORTAL_NETMASK_D   0

static const char *TAG = "gateway_main";

typedef enum {
    LOG_LEVEL_INFO = 0,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_DEBUG,
} app_log_level_t;

typedef struct {
    app_log_level_t level;
    char module[20];
    char message[160];
} app_log_event_t;

typedef struct {
    uint32_t sequence;
    int64_t timestamp_us;
    uint16_t temperature_x10;
    uint16_t humidity_x10;
    uint16_t pressure_hpa;
    uint16_t voltage_mv;
    uint16_t current_ma;
    uint16_t frequency_x10;
    uint16_t motor_speed_rpm;
    uint16_t alarm_status;
    uint16_t device_status;
} modbus_raw_frame_t;

typedef struct {
    uint32_t sequence;
    int64_t timestamp_us;
    float temperature_c;
    float humidity_percent;
    uint16_t pressure_hpa;
    float voltage_v;
    float current_a;
    float power_w;
    float frequency_hz;
    uint16_t motor_speed_rpm;
    uint16_t alarm_status;
    uint16_t device_status;
    char quality[12];
} engineering_sample_t;

typedef struct {
    QueueHandle_t log_queue;
    QueueHandle_t raw_queue;
    QueueHandle_t sample_queue;
    EventGroupHandle_t system_events;
    esp_netif_t *eth_netif;
    esp_eth_handle_t eth_handle;
    esp_netif_ip_info_t network_ip;
    esp_netif_dns_info_t network_dns;
    esp_netif_dns_info_t network_backup_dns;
} app_context_t;

typedef struct {
    char location[512];
} http_response_context_t;

static app_context_t s_app;
static esp_eth_mac_t *s_eth_mac;
static esp_eth_phy_t *s_eth_phy;
static esp_eth_netif_glue_handle_t s_eth_glue;
static httpd_handle_t s_web_server;

static void post_log(app_context_t *ctx,
                     app_log_level_t level,
                     const char *module,
                     const char *format,
                     ...)
{
    app_log_event_t event = {
        .level = level,
    };
    va_list args;

    (void)snprintf(event.module, sizeof(event.module), "%s", module);

    va_start(args, format);
    (void)vsnprintf(event.message, sizeof(event.message), format, args);
    va_end(args);

    if ((ctx == NULL) || (ctx->log_queue == NULL) ||
        (xQueueSend(ctx->log_queue, &event, 0U) != pdPASS)) {
        ESP_LOGI(module, "%s", event.message);
    }
}

static void log_event_write(const app_log_event_t *event)
{
    switch (event->level) {
    case LOG_LEVEL_ERROR:
        ESP_LOGE(event->module, "%s", event->message);
        break;
    case LOG_LEVEL_WARNING:
        ESP_LOGW(event->module, "%s", event->message);
        break;
    case LOG_LEVEL_DEBUG:
        ESP_LOGD(event->module, "%s", event->message);
        break;
    case LOG_LEVEL_INFO:
    default:
        ESP_LOGI(event->module, "%s", event->message);
        break;
    }
}

static void log_firmware_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip_info;

    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "Project: %s", app->project_name);
    ESP_LOGI(TAG, "Version: %s", app->version);
    ESP_LOGI(TAG, "Build date: %s %s", app->date, app->time);
    ESP_LOGI(TAG, "ESP-IDF: %s", app->idf_ver);
    ESP_LOGI(TAG, "Chip cores: %d", chip_info.cores);
    ESP_LOGI(TAG, "Chip revision: v%d.%d",
             chip_info.revision / 100,
             chip_info.revision % 100);
}

static void log_ethernet_pin_map(void)
{
    ESP_LOGI(TAG, "Board: %s v%s", BOARD_NAME, BOARD_VERSION);
    ESP_LOGI(TAG, "W5500 CS: GPIO%d", (int)BOARD_ETH_CS_PIN);
    ESP_LOGI(TAG, "W5500 RESET: GPIO%d", (int)BOARD_ETH_RST_PIN);
    ESP_LOGI(TAG, "W5500 INT: GPIO%d", (int)BOARD_ETH_INT_PIN);
    ESP_LOGI(TAG, "SPI MOSI: GPIO%d", (int)BOARD_SPI_MOSI_PIN);
    ESP_LOGI(TAG, "SPI MISO: GPIO%d", (int)BOARD_SPI_MISO_PIN);
    ESP_LOGI(TAG, "SPI SCK: GPIO%d", (int)BOARD_SPI_SCK_PIN);
}

static bool initialize_esp_network_stack(void)
{
    esp_err_t err = esp_netif_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "ESP network stack initialized");
    return true;
}

static bool initialize_nvs_storage(void)
{
    esp_err_t err = nvs_flash_init();

    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
            return false;
        }

        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "NVS storage initialized");
    return true;
}

static void set_default_network_profile(app_context_t *ctx)
{
    ctx->network_ip.ip.addr = ESP_IP4TOADDR(NETWORK_STATIC_IP_A,
                                            NETWORK_STATIC_IP_B,
                                            NETWORK_STATIC_IP_C,
                                            NETWORK_STATIC_IP_D);
    ctx->network_ip.gw.addr = ESP_IP4TOADDR(NETWORK_STATIC_GW_A,
                                            NETWORK_STATIC_GW_B,
                                            NETWORK_STATIC_GW_C,
                                            NETWORK_STATIC_GW_D);
    ctx->network_ip.netmask.addr = ESP_IP4TOADDR(NETWORK_STATIC_NETMASK_A,
                                                 NETWORK_STATIC_NETMASK_B,
                                                 NETWORK_STATIC_NETMASK_C,
                                                 NETWORK_STATIC_NETMASK_D);
    ctx->network_dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(NETWORK_STATIC_DNS_A,
                                                        NETWORK_STATIC_DNS_B,
                                                        NETWORK_STATIC_DNS_C,
                                                        NETWORK_STATIC_DNS_D);
    ctx->network_dns.ip.type = ESP_IPADDR_TYPE_V4;
    ctx->network_backup_dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(NETWORK_STATIC_BACKUP_DNS_A,
                                                               NETWORK_STATIC_BACKUP_DNS_B,
                                                               NETWORK_STATIC_BACKUP_DNS_C,
                                                               NETWORK_STATIC_BACKUP_DNS_D);
    ctx->network_backup_dns.ip.type = ESP_IPADDR_TYPE_V4;
}

static bool parse_ipv4_value(const char *value, esp_ip4_addr_t *address)
{
    return (value != NULL) &&
           (address != NULL) &&
           (esp_netif_str_to_ip4(value, address) == ESP_OK);
}

static void ipv4_to_string(const esp_ip4_addr_t *address, char *buffer, size_t buffer_size)
{
    if ((address == NULL) || (buffer == NULL) || (buffer_size == 0U)) {
        return;
    }

    (void)snprintf(buffer, buffer_size, IPSTR, IP2STR(address));
}

static void load_nvs_ip(nvs_handle_t nvs, const char *key, esp_ip4_addr_t *address)
{
    char value[16];
    size_t value_size = sizeof(value);

    if ((nvs_get_str(nvs, key, value, &value_size) == ESP_OK) &&
        parse_ipv4_value(value, address)) {
        ESP_LOGI(TAG, "Loaded network %s=%s", key, value);
    }
}

static void load_network_profile(app_context_t *ctx)
{
    nvs_handle_t nvs;
    esp_err_t err;

    set_default_network_profile(ctx);

    err = nvs_open(NVS_NAMESPACE_NETWORK, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Using default static network profile");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open network profile failed: %s", esp_err_to_name(err));
        return;
    }

    load_nvs_ip(nvs, NVS_KEY_IP, &ctx->network_ip.ip);
    load_nvs_ip(nvs, NVS_KEY_GATEWAY, &ctx->network_ip.gw);
    load_nvs_ip(nvs, NVS_KEY_NETMASK, &ctx->network_ip.netmask);
    load_nvs_ip(nvs, NVS_KEY_DNS, &ctx->network_dns.ip.u_addr.ip4);
    load_nvs_ip(nvs, NVS_KEY_BACKUP_DNS, &ctx->network_backup_dns.ip.u_addr.ip4);
    nvs_close(nvs);
}

static bool network_profile_is_unconfigured(const app_context_t *ctx)
{
    return (ctx == NULL) || (ctx->network_ip.ip.addr == 0U);
}

static bool save_network_profile(const esp_netif_ip_info_t *ip_info,
                                 const esp_netif_dns_info_t *dns_info,
                                 const esp_netif_dns_info_t *backup_dns_info)
{
    nvs_handle_t nvs;
    char ip[16];
    char gateway[16];
    char netmask[16];
    char dns[16];
    char backup_dns[16];
    esp_err_t err = nvs_open(NVS_NAMESPACE_NETWORK, NVS_READWRITE, &nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open network profile for write failed: %s", esp_err_to_name(err));
        return false;
    }

    ipv4_to_string(&ip_info->ip, ip, sizeof(ip));
    ipv4_to_string(&ip_info->gw, gateway, sizeof(gateway));
    ipv4_to_string(&ip_info->netmask, netmask, sizeof(netmask));
    ipv4_to_string(&dns_info->ip.u_addr.ip4, dns, sizeof(dns));
    ipv4_to_string(&backup_dns_info->ip.u_addr.ip4, backup_dns, sizeof(backup_dns));

    err = nvs_set_str(nvs, NVS_KEY_IP, ip);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_GATEWAY, gateway);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_NETMASK, netmask);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_DNS, dns);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_BACKUP_DNS, backup_dns);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving network profile failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG,
             "Saved static network profile: ip=%s gateway=%s mask=%s dns=%s backup=%s",
             ip,
             gateway,
             netmask,
             dns,
             backup_dns);
    return true;
}

static void ethernet_event_handler(void *arg,
                                   esp_event_base_t event_base,
                                   int32_t event_id,
                                   void *event_data)
{
    app_context_t *ctx = (app_context_t *)arg;
    uint8_t mac_addr[6] = {0};

    (void)event_base;

    switch (event_id) {
    case ETHERNET_EVENT_START:
        post_log(ctx, LOG_LEVEL_INFO, "ethernet", "Ethernet driver started");
        break;

    case ETHERNET_EVENT_CONNECTED:
        (void)esp_eth_ioctl(ctx->eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        xEventGroupSetBits(ctx->system_events, APP_EVENT_ETHERNET_LINK_UP | APP_EVENT_NETWORK_READY);
        post_log(ctx,
                 LOG_LEVEL_INFO,
                 "ethernet",
                 "W5500 link up, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0],
                 mac_addr[1],
                 mac_addr[2],
                 mac_addr[3],
                 mac_addr[4],
                 mac_addr[5]);
        if (network_profile_is_unconfigured(ctx)) {
            post_log(ctx,
                     LOG_LEVEL_INFO,
                     "network",
                     "Web config portal active at %u.%u.%u.%u",
                     WEB_CONFIG_PORTAL_IP_A,
                     WEB_CONFIG_PORTAL_IP_B,
                     WEB_CONFIG_PORTAL_IP_C,
                     WEB_CONFIG_PORTAL_IP_D);
        } else {
            post_log(ctx,
                     LOG_LEVEL_INFO,
                     "network",
                     "Static Ethernet address active: " IPSTR,
                     IP2STR(&ctx->network_ip.ip));
        }
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        xEventGroupClearBits(ctx->system_events, APP_EVENT_ETHERNET_LINK_UP | APP_EVENT_NETWORK_READY);
        post_log(ctx, LOG_LEVEL_WARNING, "ethernet", "W5500 link down");
        break;

    case ETHERNET_EVENT_STOP:
        xEventGroupClearBits(ctx->system_events, APP_EVENT_ETHERNET_LINK_UP | APP_EVENT_NETWORK_READY);
        post_log(ctx, LOG_LEVEL_WARNING, "ethernet", "Ethernet driver stopped");
        break;

    default:
        break;
    }
}

static void got_ip_event_handler(void *arg,
                                 esp_event_base_t event_base,
                                 int32_t event_id,
                                 void *event_data)
{
    app_context_t *ctx = (app_context_t *)arg;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    (void)event_base;
    (void)event_id;

    xEventGroupSetBits(ctx->system_events, APP_EVENT_NETWORK_READY);
    post_log(ctx,
             LOG_LEVEL_INFO,
             "network",
             "Ethernet DHCP address: " IPSTR ", gateway: " IPSTR ", netmask: " IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.gw),
             IP2STR(&event->ip_info.netmask));
}

static bool configure_static_ethernet_ip(app_context_t *ctx)
{
    esp_netif_ip_info_t active_ip = ctx->network_ip;
    esp_netif_dns_info_t active_dns = ctx->network_dns;
    esp_netif_dns_info_t active_backup_dns = ctx->network_backup_dns;
    esp_err_t err;

    if (network_profile_is_unconfigured(ctx)) {
        active_ip.ip.addr = ESP_IP4TOADDR(WEB_CONFIG_PORTAL_IP_A,
                                          WEB_CONFIG_PORTAL_IP_B,
                                          WEB_CONFIG_PORTAL_IP_C,
                                          WEB_CONFIG_PORTAL_IP_D);
        active_ip.gw.addr = active_ip.ip.addr;
        active_ip.netmask.addr = ESP_IP4TOADDR(WEB_CONFIG_PORTAL_NETMASK_A,
                                               WEB_CONFIG_PORTAL_NETMASK_B,
                                               WEB_CONFIG_PORTAL_NETMASK_C,
                                               WEB_CONFIG_PORTAL_NETMASK_D);
        active_dns.ip.u_addr.ip4.addr = active_ip.ip.addr;
        active_dns.ip.type = ESP_IPADDR_TYPE_V4;
        active_backup_dns.ip.u_addr.ip4.addr = active_ip.ip.addr;
        active_backup_dns.ip.type = ESP_IPADDR_TYPE_V4;
        ESP_LOGI(TAG,
                 "Network profile is 0.0.0.0; Ethernet setup portal will use " IPSTR,
                 IP2STR(&active_ip.ip));
    }

    err = esp_netif_dhcpc_stop(ctx->eth_netif);

    if ((err != ESP_OK) && (err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)) {
        ESP_LOGE(TAG, "esp_netif_dhcpc_stop failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_netif_set_ip_info(ctx->eth_netif, &active_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_netif_set_dns_info(ctx->eth_netif, ESP_NETIF_DNS_MAIN, &active_dns);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_dns_info main failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_netif_set_dns_info(ctx->eth_netif, ESP_NETIF_DNS_BACKUP, &active_backup_dns);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_dns_info backup failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG,
             "Ethernet static IP configured: " IPSTR ", gateway: " IPSTR ", netmask: " IPSTR,
             IP2STR(&active_ip.ip),
             IP2STR(&active_ip.gw),
             IP2STR(&active_ip.netmask));

    return true;
}

static bool initialize_w5500_ethernet(app_context_t *ctx)
{
    esp_err_t err;
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    spi_bus_config_t bus_config = {
        .miso_io_num = BOARD_SPI_MISO_PIN,
        .mosi_io_num = BOARD_SPI_MOSI_PIN,
        .sclk_io_num = BOARD_SPI_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    spi_device_interface_config_t spi_device_config = {
        .mode = 0,
        .clock_speed_hz = W5500_SPI_CLOCK_HZ,
        .spics_io_num = BOARD_ETH_CS_PIN,
        .queue_size = 20,
    };
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &spi_device_config);
    esp_eth_config_t eth_config;
    uint8_t eth_mac[6] = {0};

    ctx->eth_netif = esp_netif_new(&netif_config);
    if (ctx->eth_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        return false;
    }

    err = gpio_install_isr_service(0);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return false;
    }

    err = spi_bus_initialize(W5500_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return false;
    }

    mac_config.rx_task_stack_size = 4096;
    phy_config.phy_addr = W5500_PHY_ADDR;
    phy_config.reset_gpio_num = BOARD_ETH_RST_PIN;
    w5500_config.int_gpio_num = BOARD_ETH_INT_PIN;

    s_eth_mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    s_eth_phy = esp_eth_phy_new_w5500(&phy_config);
    if ((s_eth_mac == NULL) || (s_eth_phy == NULL)) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC/PHY");
        return false;
    }

    eth_config = (esp_eth_config_t)ETH_DEFAULT_CONFIG(s_eth_mac, s_eth_phy);
    err = esp_eth_driver_install(&eth_config, &ctx->eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_read_mac(eth_mac, ESP_MAC_ETH);
    if (err == ESP_OK) {
        err = esp_eth_ioctl(ctx->eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet MAC address setup failed: %s", esp_err_to_name(err));
        return false;
    }

    s_eth_glue = esp_eth_new_netif_glue(ctx->eth_handle);
    if (s_eth_glue == NULL) {
        ESP_LOGE(TAG, "esp_eth_new_netif_glue failed");
        return false;
    }

    err = esp_netif_attach(ctx->eth_netif, s_eth_glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, ethernet_event_handler, ctx);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler, ctx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet event handler registration failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!configure_static_ethernet_ip(ctx)) {
        return false;
    }

    err = esp_eth_start(ctx->eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "W5500 Ethernet interface started with static IP profile");
    return true;
}

static modbus_raw_frame_t make_simulated_modbus_frame(uint32_t sequence)
{
    const uint16_t cycle = (uint16_t)(sequence % 60U);

    return (modbus_raw_frame_t) {
        .sequence = sequence,
        .timestamp_us = esp_timer_get_time(),
        .temperature_x10 = (uint16_t)(250U + cycle),
        .humidity_x10 = (uint16_t)(600U + (cycle / 2U)),
        .pressure_hpa = (uint16_t)(1005U + (cycle % 8U)),
        .voltage_mv = (uint16_t)(3720U + (cycle % 20U)),
        .current_ma = (uint16_t)(420U + (cycle * 3U)),
        .frequency_x10 = (uint16_t)(500U + (cycle % 3U)),
        .motor_speed_rpm = (uint16_t)(1450U + (cycle * 2U)),
        .alarm_status = 0U,
        .device_status = 1U,
    };
}

static engineering_sample_t convert_registers_to_engineering(const modbus_raw_frame_t *raw)
{
    const float voltage_v = ((float)raw->voltage_mv) / 1000.0f;
    const float current_a = ((float)raw->current_ma) / 1000.0f;

    engineering_sample_t sample = {
        .sequence = raw->sequence,
        .timestamp_us = raw->timestamp_us,
        .temperature_c = ((float)raw->temperature_x10) / 10.0f,
        .humidity_percent = ((float)raw->humidity_x10) / 10.0f,
        .pressure_hpa = raw->pressure_hpa,
        .voltage_v = voltage_v,
        .current_a = current_a,
        .power_w = voltage_v * current_a,
        .frequency_hz = ((float)raw->frequency_x10) / 10.0f,
        .motor_speed_rpm = raw->motor_speed_rpm,
        .alarm_status = raw->alarm_status,
        .device_status = raw->device_status,
    };

    (void)snprintf(sample.quality, sizeof(sample.quality), "%s", "GOOD");
    return sample;
}

static void build_json_payload(const engineering_sample_t *sample,
                               char *buffer,
                               size_t buffer_size)
{
    (void)snprintf(buffer,
                   buffer_size,
                   "{\"device\":\"%s\","
                   "\"sequence\":%lu,"
                   "\"timestamp_us\":%lld,"
                   "\"temperature\":%.1f,"
                   "\"humidity\":%.1f,"
                   "\"pressure\":%u,"
                   "\"voltage\":%.3f,"
                   "\"current\":%.3f,"
                   "\"power\":%.3f,"
                   "\"frequency\":%.1f,"
                   "\"motor_speed\":%u,"
                   "\"status\":%u,"
                   "\"alarm\":%u,"
                   "\"quality\":\"%s\"}",
                   GATEWAY_DEVICE_NAME,
                   (unsigned long)sample->sequence,
                   (long long)sample->timestamp_us,
                   (double)sample->temperature_c,
                   (double)sample->humidity_percent,
                   (unsigned int)sample->pressure_hpa,
                   (double)sample->voltage_v,
                   (double)sample->current_a,
                   (double)sample->power_w,
                   (double)sample->frequency_hz,
                   (unsigned int)sample->motor_speed_rpm,
                   (unsigned int)sample->device_status,
                   (unsigned int)sample->alarm_status,
                   sample->quality);
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_context_t *response = (http_response_context_t *)event->user_data;

    if ((event->event_id == HTTP_EVENT_ON_HEADER) &&
        (response != NULL) &&
        (event->header_key != NULL) &&
        (event->header_value != NULL) &&
        (strcasecmp(event->header_key, "Location") == 0)) {
        (void)snprintf(response->location,
                       sizeof(response->location),
                       "%s",
                       event->header_value);
    }

    return ESP_OK;
}

static esp_err_t send_https_request_once(const char *url,
                                         esp_http_client_method_t method,
                                         const char *payload,
                                         int *status_code,
                                         http_response_context_t *response)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = HTTP_CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
        .event_handler = http_event_handler,
        .user_data = response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err;

    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "User-Agent", "Industrial-Ethernet-IoT-Gateway/1.0");
    }
    if ((err == ESP_OK) && (payload != NULL)) {
        err = esp_http_client_set_post_field(client, payload, (int)strlen(payload));
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }

    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }
    if ((response != NULL) &&
        (status_code != NULL) &&
        ((*status_code == 301) || (*status_code == 302) || (*status_code == 303) ||
         (*status_code == 307) || (*status_code == 308))) {
        char *location = NULL;

        if ((esp_http_client_get_header(client, "Location", &location) == ESP_OK) &&
            (location != NULL)) {
            (void)snprintf(response->location,
                           sizeof(response->location),
                           "%s",
                           location);
        }
    }

    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t send_google_sheets_post(const char *payload, int *status_code)
{
    return send_https_request_once(CLOUD_GOOGLE_SCRIPT_URL,
                                   HTTP_METHOD_POST,
                                   payload,
                                   status_code,
                                   NULL);
}

static bool google_sheets_status_is_ok(int status_code)
{
    return ((status_code >= 200) && (status_code < 300)) ||
           (status_code == 301) ||
           (status_code == 302) ||
           (status_code == 303);
}

static bool form_get_ipv4(const char *body, const char *key, esp_ip4_addr_t *address)
{
    char value[16];

    if ((httpd_query_key_value(body, key, value, sizeof(value)) != ESP_OK) ||
        !parse_ipv4_value(value, address)) {
        return false;
    }

    return true;
}

static void reboot_after_config_save_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(1500U));
    esp_restart();
}

static esp_err_t web_config_get_handler(httpd_req_t *req)
{
    app_context_t *ctx = (app_context_t *)req->user_ctx;
    char ip[16];
    char gateway[16];
    char netmask[16];
    char dns[16];
    char backup_dns[16];
    char current_url_ip[16];
    char page[1800];
    esp_netif_ip_info_t active_ip = ctx->network_ip;

    ipv4_to_string(&ctx->network_ip.ip, ip, sizeof(ip));
    ipv4_to_string(&ctx->network_ip.gw, gateway, sizeof(gateway));
    ipv4_to_string(&ctx->network_ip.netmask, netmask, sizeof(netmask));
    ipv4_to_string(&ctx->network_dns.ip.u_addr.ip4, dns, sizeof(dns));
    ipv4_to_string(&ctx->network_backup_dns.ip.u_addr.ip4, backup_dns, sizeof(backup_dns));
    if (network_profile_is_unconfigured(ctx)) {
        (void)esp_netif_get_ip_info(ctx->eth_netif, &active_ip);
    }
    ipv4_to_string(&active_ip.ip, current_url_ip, sizeof(current_url_ip));

    (void)snprintf(page,
                   sizeof(page),
                   "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                   "<title>Gateway Network</title><style>"
                   "body{font-family:Arial,sans-serif;margin:24px;max-width:520px}"
                   "label{display:block;margin:14px 0 6px;font-weight:600}"
                   "input{box-sizing:border-box;width:100%%;padding:10px;font-size:16px}"
                   "button{margin-top:18px;padding:12px 16px;font-size:16px}"
                   ".meta{color:#555;font-size:14px;margin-top:16px}"
                   "</style></head><body>"
                   "<h1>Gateway Network</h1>"
                   "<form method=\"post\" action=\"/network\">"
                   "<label>Static IP</label><input name=\"ip\" value=\"%s\" inputmode=\"decimal\">"
                   "<label>Gateway</label><input name=\"gw\" value=\"%s\" inputmode=\"decimal\">"
                   "<label>Netmask</label><input name=\"mask\" value=\"%s\" inputmode=\"decimal\">"
                   "<label>DNS</label><input name=\"dns\" value=\"%s\" inputmode=\"decimal\">"
                   "<label>Backup DNS</label><input name=\"bdns\" value=\"%s\" inputmode=\"decimal\">"
                   "<button type=\"submit\">Save and Reboot</button>"
                   "</form><p class=\"meta\">Current URL: http://%s/</p>"
                   "</body></html>",
                   ip,
                   gateway,
                   netmask,
                   dns,
                   backup_dns,
                   current_url_ip);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_config_post_handler(httpd_req_t *req)
{
    char body[WEB_CONFIG_POST_MAX_BYTES + 1U];
    int received;
    int remaining = req->content_len;
    int offset = 0;
    esp_netif_ip_info_t ip_info;
    esp_netif_dns_info_t dns_info = {0};
    esp_netif_dns_info_t backup_dns_info = {0};

    if ((remaining <= 0) || (remaining > WEB_CONFIG_POST_MAX_BYTES)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form size");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        received = httpd_req_recv(req, &body[offset], remaining);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive form");
            return ESP_FAIL;
        }

        offset += received;
        remaining -= received;
    }
    body[offset] = '\0';

    if (!form_get_ipv4(body, NVS_KEY_IP, &ip_info.ip) ||
        !form_get_ipv4(body, NVS_KEY_GATEWAY, &ip_info.gw) ||
        !form_get_ipv4(body, NVS_KEY_NETMASK, &ip_info.netmask) ||
        !form_get_ipv4(body, NVS_KEY_DNS, &dns_info.ip.u_addr.ip4) ||
        !form_get_ipv4(body, NVS_KEY_BACKUP_DNS, &backup_dns_info.ip.u_addr.ip4)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid IPv4 address");
        return ESP_FAIL;
    }

    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    backup_dns_info.ip.type = ESP_IPADDR_TYPE_V4;

    if (!save_network_profile(&ip_info, &dns_info, &backup_dns_info)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save network profile");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
                       "<!doctype html><html><body><h1>Saved</h1>"
                       "<p>Gateway is rebooting with the new network profile.</p>"
                       "</body></html>");

    (void)xTaskCreate(reboot_after_config_save_task,
                      "config_reboot",
                      2048U,
                      NULL,
                      APP_PRIORITY_WATCHDOG,
                      NULL);
    return ESP_OK;
}

static bool start_web_config_server(app_context_t *ctx)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_netif_ip_info_t active_ip = ctx->network_ip;
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_config_get_handler,
        .user_ctx = ctx,
    };
    httpd_uri_t network_uri = {
        .uri = "/network",
        .method = HTTP_POST,
        .handler = web_config_post_handler,
        .user_ctx = ctx,
    };
    esp_err_t err;

    if (s_web_server != NULL) {
        return true;
    }

    config.server_port = 80;
    config.max_uri_handlers = 4;

    err = httpd_start(&s_web_server, &config);
    if (err != ESP_OK) {
        post_log(ctx, LOG_LEVEL_ERROR, "web", "Config web server start failed: %s", esp_err_to_name(err));
        s_web_server = NULL;
        return false;
    }

    err = httpd_register_uri_handler(s_web_server, &root_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_web_server, &network_uri);
    }
    if (err != ESP_OK) {
        post_log(ctx, LOG_LEVEL_ERROR, "web", "Config web route registration failed: %s", esp_err_to_name(err));
        httpd_stop(s_web_server);
        s_web_server = NULL;
        return false;
    }

    if (network_profile_is_unconfigured(ctx)) {
        (void)esp_netif_get_ip_info(ctx->eth_netif, &active_ip);
    }

    post_log(ctx,
             LOG_LEVEL_INFO,
             "web",
             "Config web server ready at http://" IPSTR "/",
             IP2STR(&active_ip.ip));
    return true;
}

static void logger_task(void *argument)
{
    app_context_t *ctx = (app_context_t *)argument;
    app_log_event_t event;

    for (;;) {
        if (xQueueReceive(ctx->log_queue, &event, portMAX_DELAY) == pdPASS) {
            log_event_write(&event);
        }
    }
}

static void network_manager_task(void *argument)
{
    app_context_t *ctx = (app_context_t *)argument;

    post_log(ctx, LOG_LEVEL_INFO, "network", "Network manager started");

    for (;;) {
        const EventBits_t bits = xEventGroupGetBits(ctx->system_events);

        if ((bits & APP_EVENT_NETWORK_READY) != 0U) {
            post_log(ctx, LOG_LEVEL_INFO, "network", "Ethernet network is ready");
            (void)start_web_config_server(ctx);
        } else {
            post_log(ctx, LOG_LEVEL_WARNING, "network", "Waiting for Ethernet link");
        }

        vTaskDelay(pdMS_TO_TICKS(10000U));
    }
}

static void ethernet_monitor_task(void *argument)
{
    app_context_t *ctx = (app_context_t *)argument;
    bool link_was_up = false;

    post_log(ctx, LOG_LEVEL_INFO, "ethernet", "Ethernet monitor started");

    for (;;) {
        const EventBits_t bits = xEventGroupGetBits(ctx->system_events);
        const bool link_is_up = (bits & APP_EVENT_ETHERNET_LINK_UP) != 0U;

        if (link_is_up != link_was_up) {
            if (link_is_up) {
                xEventGroupSetBits(ctx->system_events, APP_EVENT_ETHERNET_LINK_UP);
                post_log(ctx, LOG_LEVEL_INFO, "ethernet", "W5500 link state: UP");
            } else {
                xEventGroupClearBits(ctx->system_events, APP_EVENT_ETHERNET_LINK_UP);
                post_log(ctx, LOG_LEVEL_WARNING, "ethernet", "W5500 link state: DOWN");
            }

            link_was_up = link_is_up;
        }

        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void modbus_poll_task(void *argument)
{
    app_context_t *ctx = (app_context_t *)argument;
    uint32_t sequence = 0U;

    post_log(ctx, LOG_LEVEL_INFO, "modbus", "Modbus poll task started");

    for (;;) {
        (void)xEventGroupWaitBits(ctx->system_events,
                                  APP_EVENT_NETWORK_READY | APP_EVENT_ETHERNET_LINK_UP,
                                  pdFALSE,
                                  pdTRUE,
                                  portMAX_DELAY);

        modbus_raw_frame_t frame = make_simulated_modbus_frame(sequence);

        if (xQueueSend(ctx->raw_queue, &frame, pdMS_TO_TICKS(100U)) == pdPASS) {
            xEventGroupSetBits(ctx->system_events, APP_EVENT_MODBUS_HEALTHY);
            post_log(ctx, LOG_LEVEL_DEBUG, "modbus", "Polled simulated PLC frame %lu",
                     (unsigned long)sequence);
            sequence++;
        } else {
            post_log(ctx, LOG_LEVEL_WARNING, "modbus", "Raw data queue full");
        }

        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void data_processing_task(void *argument)
{
    app_context_t *ctx = (app_context_t *)argument;
    modbus_raw_frame_t raw;

    post_log(ctx, LOG_LEVEL_INFO, "processing", "Data processing task started");

    for (;;) {
        if (xQueueReceive(ctx->raw_queue, &raw, portMAX_DELAY) == pdPASS) {
            engineering_sample_t sample = convert_registers_to_engineering(&raw);

            if (xQueueSend(ctx->sample_queue, &sample, pdMS_TO_TICKS(100U)) == pdPASS) {
                xEventGroupSetBits(ctx->system_events, APP_EVENT_PROCESSING_HEALTHY);
            } else {
                engineering_sample_t dropped_sample;

                (void)xQueueReceive(ctx->sample_queue, &dropped_sample, 0U);
                if (xQueueSend(ctx->sample_queue, &sample, 0U) != pdPASS) {
                    post_log(ctx, LOG_LEVEL_WARNING, "processing", "Engineering sample queue full");
                }
            }
        }
    }
}

static void http_upload_task(void *argument)
{
    app_context_t *ctx = (app_context_t *)argument;
    engineering_sample_t sample;
    engineering_sample_t latest_sample;
    bool latest_sample_valid = false;
    uint32_t fallback_sequence = 0U;
    TickType_t next_upload_tick = xTaskGetTickCount();
    char payload[384];

    post_log(ctx,
             LOG_LEVEL_INFO,
             "http",
             "HTTP upload task started: https://%s%s",
             CLOUD_GOOGLE_SCRIPT_HOST,
             CLOUD_GOOGLE_SCRIPT_PATH);

    for (;;) {
        TickType_t now = xTaskGetTickCount();

        while ((int32_t)(next_upload_tick - now) > 0) {
            const TickType_t wait_ticks = next_upload_tick - now;

            if (xQueueReceive(ctx->sample_queue, &sample, wait_ticks) == pdPASS) {
                latest_sample = sample;
                latest_sample_valid = true;
                now = xTaskGetTickCount();
                continue;
            }

            now = xTaskGetTickCount();
        }

        if (!latest_sample_valid) {
            post_log(ctx, LOG_LEVEL_INFO, "http", "Waiting for first sample before upload");
            if (xQueueReceive(ctx->sample_queue, &latest_sample, pdMS_TO_TICKS(2000U)) == pdPASS) {
                latest_sample_valid = true;
            } else {
                modbus_raw_frame_t fallback_frame = make_simulated_modbus_frame(fallback_sequence++);

                latest_sample = convert_registers_to_engineering(&fallback_frame);
                latest_sample_valid = true;
                post_log(ctx,
                         LOG_LEVEL_WARNING,
                         "http",
                         "No processed sample available; posting fallback sample %lu",
                         (unsigned long)latest_sample.sequence);
            }
        }

        if (latest_sample_valid) {
            int status_code = 0;
            const TickType_t upload_start_tick = xTaskGetTickCount();

            (void)xEventGroupWaitBits(ctx->system_events,
                                      APP_EVENT_NETWORK_READY | APP_EVENT_ETHERNET_LINK_UP,
                                      pdFALSE,
                                      pdTRUE,
                                      portMAX_DELAY);

            build_json_payload(&latest_sample, payload, sizeof(payload));
            post_log(ctx,
                     LOG_LEVEL_INFO,
                     "http",
                     "Sending HTTPS POST to %s:%u%s",
                     CLOUD_GOOGLE_SCRIPT_HOST,
                     (unsigned int)CLOUD_GOOGLE_SCRIPT_PORT,
                     CLOUD_GOOGLE_SCRIPT_PATH);
            post_log(ctx,
                     LOG_LEVEL_INFO,
                     "http",
                     "JSON payload bytes=%u sequence=%lu",
                     (unsigned int)strlen(payload),
                     (unsigned long)latest_sample.sequence);

            esp_err_t err = send_google_sheets_post(payload, &status_code);
            const TickType_t upload_end_tick = xTaskGetTickCount();
            const uint32_t elapsed_ms = (uint32_t)((upload_end_tick - upload_start_tick) * portTICK_PERIOD_MS);

            if ((err == ESP_OK) && google_sheets_status_is_ok(status_code)) {
                xEventGroupSetBits(ctx->system_events, APP_EVENT_HTTP_HEALTHY);
                post_log(ctx,
                         LOG_LEVEL_INFO,
                         "http",
                         "Google Sheets upload accepted, status=%d",
                         status_code);
            } else {
                xEventGroupClearBits(ctx->system_events, APP_EVENT_HTTP_HEALTHY);
                post_log(ctx,
                         LOG_LEVEL_ERROR,
                         "http",
                         "Google Sheets upload failed, err=%s, status=%d",
                         esp_err_to_name(err),
                         status_code);
            }

            post_log(ctx,
                     LOG_LEVEL_INFO,
                     "http",
                     "Upload execution time=%lu ms; next hourly upload remains phase-aligned",
                     (unsigned long)elapsed_ms);
            latest_sample_valid = false;
        }

        do {
            next_upload_tick += pdMS_TO_TICKS(HTTP_UPLOAD_PERIOD_MS);
        } while ((int32_t)(next_upload_tick - xTaskGetTickCount()) <= 0);
    }
}

static void diagnostics_task(void *argument)
{
    app_context_t *ctx = (app_context_t *)argument;

    post_log(ctx, LOG_LEVEL_INFO, "diagnostics", "Diagnostics task started");

    for (;;) {
        xEventGroupSetBits(ctx->system_events, APP_EVENT_DIAGNOSTICS_HEALTHY);
        post_log(ctx,
                 LOG_LEVEL_INFO,
                 "diagnostics",
                 "heap=%u raw_q=%u sample_q=%u log_q=%u",
                 (unsigned int)esp_get_free_heap_size(),
                 (unsigned int)uxQueueMessagesWaiting(ctx->raw_queue),
                 (unsigned int)uxQueueMessagesWaiting(ctx->sample_queue),
                 (unsigned int)uxQueueMessagesWaiting(ctx->log_queue));

        vTaskDelay(pdMS_TO_TICKS(5000U));
    }
}

static void watchdog_task(void *argument)
{
    app_context_t *ctx = (app_context_t *)argument;

    post_log(ctx, LOG_LEVEL_INFO, "watchdog", "Watchdog supervisor started");

    for (;;) {
        const EventBits_t bits = xEventGroupGetBits(ctx->system_events);

        if ((bits & APP_EVENT_NETWORK_READY) == 0U) {
            post_log(ctx, LOG_LEVEL_WARNING, "watchdog", "Network manager is not ready");
        }

        if ((bits & APP_EVENT_ETHERNET_LINK_UP) == 0U) {
            post_log(ctx, LOG_LEVEL_WARNING, "watchdog", "Ethernet link is down");
        }

        vTaskDelay(pdMS_TO_TICKS(3000U));
    }
}

static void led_status_task(void *argument)
{
    app_context_t *ctx = (app_context_t *)argument;
    uint32_t heartbeat = 0U;

    post_log(ctx, LOG_LEVEL_INFO, "led", "LED status task started");

    for (;;) {
        xEventGroupSetBits(ctx->system_events, APP_EVENT_LED_HEALTHY);
        post_log(ctx, LOG_LEVEL_DEBUG, "led", "Heartbeat %lu", (unsigned long)heartbeat);
        heartbeat++;
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static bool create_gateway_task(TaskFunction_t task,
                                const char *name,
                                uint32_t stack_size,
                                UBaseType_t priority,
                                app_context_t *ctx)
{
    const BaseType_t result = xTaskCreate(task,
                                          name,
                                          stack_size,
                                          ctx,
                                          priority,
                                          NULL);

    return result == pdPASS;
}

static bool create_gateway_objects(app_context_t *ctx)
{
    ctx->log_queue = xQueueCreate(APP_LOG_QUEUE_LENGTH, sizeof(app_log_event_t));
    ctx->raw_queue = xQueueCreate(APP_RAW_QUEUE_LENGTH, sizeof(modbus_raw_frame_t));
    ctx->sample_queue = xQueueCreate(APP_SAMPLE_QUEUE_LENGTH, sizeof(engineering_sample_t));
    ctx->system_events = xEventGroupCreate();

    return (ctx->log_queue != NULL) &&
           (ctx->raw_queue != NULL) &&
           (ctx->sample_queue != NULL) &&
           (ctx->system_events != NULL);
}

static bool start_gateway_tasks(app_context_t *ctx)
{
    bool ok = true;

    ok = ok && create_gateway_task(logger_task, "logger", APP_TASK_STACK_LOGGER, APP_PRIORITY_LOGGER, ctx);
    ok = ok && create_gateway_task(network_manager_task, "network_mgr", APP_TASK_STACK_NETWORK, APP_PRIORITY_NETWORK, ctx);
    ok = ok && create_gateway_task(ethernet_monitor_task, "eth_monitor", APP_TASK_STACK_ETHERNET, APP_PRIORITY_ETHERNET, ctx);
    ok = ok && create_gateway_task(modbus_poll_task, "modbus_poll", APP_TASK_STACK_MODBUS, APP_PRIORITY_MODBUS, ctx);
    ok = ok && create_gateway_task(data_processing_task, "data_proc", APP_TASK_STACK_PROCESSING, APP_PRIORITY_PROCESSING, ctx);
    ok = ok && create_gateway_task(http_upload_task, "http_upload", APP_TASK_STACK_HTTP, APP_PRIORITY_HTTP, ctx);
    ok = ok && create_gateway_task(diagnostics_task, "diagnostics", APP_TASK_STACK_DIAGNOSTICS, APP_PRIORITY_DIAGNOSTICS, ctx);
    ok = ok && create_gateway_task(watchdog_task, "watchdog", APP_TASK_STACK_WATCHDOG, APP_PRIORITY_WATCHDOG, ctx);
    ok = ok && create_gateway_task(led_status_task, "led_status", APP_TASK_STACK_LED, APP_PRIORITY_LED, ctx);

    return ok;
}

void app_main(void)
{
    memset(&s_app, 0, sizeof(s_app));

    ESP_LOGI(TAG, "Industrial Ethernet IoT Gateway boot");
    log_firmware_info();
    log_ethernet_pin_map();

    if (!initialize_nvs_storage()) {
        ESP_LOGE(TAG, "Cannot continue without NVS storage");
        return;
    }

    if (!initialize_esp_network_stack()) {
        ESP_LOGE(TAG, "Cannot continue without ESP network stack");
        return;
    }

    if (!create_gateway_objects(&s_app)) {
        ESP_LOGE(TAG, "Failed to create application queues or event group");
        return;
    }

    load_network_profile(&s_app);

    if (!initialize_w5500_ethernet(&s_app)) {
        ESP_LOGE(TAG, "Failed to initialize W5500 Ethernet");
        return;
    }

    if (!start_gateway_tasks(&s_app)) {
        ESP_LOGE(TAG, "Failed to start one or more gateway tasks");
        return;
    }

    ESP_LOGI(TAG, "Gateway task set started");
}
