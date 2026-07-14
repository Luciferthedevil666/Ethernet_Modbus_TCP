#include "board.h"
#include "config.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
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

#define APP_LOG_QUEUE_LENGTH          16U
#define APP_RAW_QUEUE_LENGTH          4U
#define APP_SAMPLE_QUEUE_LENGTH       4U

#define APP_TASK_STACK_MAIN           4096U
#define APP_TASK_STACK_NETWORK        3072U
#define APP_TASK_STACK_ETHERNET       3072U
#define APP_TASK_STACK_MODBUS         4096U
#define APP_TASK_STACK_PROCESSING     4096U
#define APP_TASK_STACK_HTTP           4096U
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
} app_context_t;

static app_context_t s_app;
static esp_eth_mac_t *s_eth_mac;
static esp_eth_phy_t *s_eth_phy;
static esp_eth_netif_glue_handle_t s_eth_glue;

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
        xEventGroupSetBits(ctx->system_events, APP_EVENT_ETHERNET_LINK_UP);
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

    err = esp_eth_start(ctx->eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "W5500 Ethernet interface started, waiting for link and DHCP");
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

static esp_err_t send_google_sheets_post(const char *payload, int *status_code)
{
    esp_http_client_config_t config = {
        .url = CLOUD_GOOGLE_SCRIPT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
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
    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(client, payload, (int)strlen(payload));
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }

    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }

    esp_http_client_cleanup(client);
    return err;
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
        } else {
            post_log(ctx, LOG_LEVEL_WARNING, "network", "Waiting for Ethernet DHCP address");
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
                post_log(ctx, LOG_LEVEL_WARNING, "processing", "Engineering sample queue full");
            }
        }
    }
}

static void http_upload_task(void *argument)
{
    app_context_t *ctx = (app_context_t *)argument;
    engineering_sample_t sample;
    char payload[384];

    post_log(ctx,
             LOG_LEVEL_INFO,
             "http",
             "HTTP upload task started: https://%s%s",
             CLOUD_GOOGLE_SCRIPT_HOST,
             CLOUD_GOOGLE_SCRIPT_PATH);

    for (;;) {
        if (xQueueReceive(ctx->sample_queue, &sample, portMAX_DELAY) == pdPASS) {
            int status_code = 0;

            build_json_payload(&sample, payload, sizeof(payload));
            post_log(ctx,
                     LOG_LEVEL_INFO,
                     "http",
                     "Sending HTTPS POST to %s:%u%s",
                     CLOUD_GOOGLE_SCRIPT_HOST,
                     (unsigned int)CLOUD_GOOGLE_SCRIPT_PORT,
                     CLOUD_GOOGLE_SCRIPT_PATH);
            post_log(ctx, LOG_LEVEL_INFO, "http", "JSON payload: %s", payload);

            esp_err_t err = send_google_sheets_post(payload, &status_code);
            if ((err == ESP_OK) && (status_code >= 200) && (status_code < 300)) {
                xEventGroupSetBits(ctx->system_events, APP_EVENT_HTTP_HEALTHY);
                post_log(ctx, LOG_LEVEL_INFO, "http", "Google Sheets upload OK, status=%d", status_code);
            } else {
                xEventGroupClearBits(ctx->system_events, APP_EVENT_HTTP_HEALTHY);
                post_log(ctx,
                         LOG_LEVEL_ERROR,
                         "http",
                         "Google Sheets upload failed, err=%s, status=%d",
                         esp_err_to_name(err),
                         status_code);
            }
        }
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

    if (!initialize_esp_network_stack()) {
        ESP_LOGE(TAG, "Cannot continue without ESP network stack");
        return;
    }

    if (!create_gateway_objects(&s_app)) {
        ESP_LOGE(TAG, "Failed to create application queues or event group");
        return;
    }

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
