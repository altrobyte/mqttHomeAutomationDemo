#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define WIFI_SSID       "sidd"
#define WIFI_PASS       "12345678"
#define WIFI_MAX_RETRY  5

#define MQTT_BROKER_URL "mqtt://broker.hivemq.com:1883"

#define TOPIC_LED_CONTROL    "altrobyte/home/led/control"
#define TOPIC_LED_STATUS     "altrobyte/home/led/status"
#define TOPIC_SYSTEM_STATUS  "altrobyte/home/system/status"

#define LED_PIN    GPIO_NUM_2

static const char *TAG = "MQTT_HOME";
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static int wifi_retry = 0;

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool led_state = false;

static void gpio_init_pins(void)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);
}

static void publish_status(const char* topic, const char* message)
{
    if (mqtt_client == NULL) return;
    esp_mqtt_client_publish(mqtt_client, topic, message, 0, 1, 1);
    ESP_LOGI(TAG, "Published -> %s : %s", topic, message);
}

static void handle_led(const char* msg, int len)
{
    char cmd[16] = {0};
    strncpy(cmd, msg, len < 15 ? len : 15);

    if (strcmp(cmd, "ON") == 0) {
        led_state = true;
        gpio_set_level(LED_PIN, 1);
        publish_status(TOPIC_LED_STATUS, "ON");
        ESP_LOGI(TAG, "LED -> ON");
    } else if (strcmp(cmd, "OFF") == 0) {
        led_state = false;
        gpio_set_level(LED_PIN, 0);
        publish_status(TOPIC_LED_STATUS, "OFF");
        ESP_LOGI(TAG, "LED -> OFF");
    }
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                                int32_t event_id, void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected!");
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_LED_CONTROL, 1);
            publish_status(TOPIC_SYSTEM_STATUS, "ONLINE");
            publish_status(TOPIC_LED_STATUS, "OFF");
            ESP_LOGI(TAG, "Ready! Publish ON/OFF to: %s", TOPIC_LED_CONTROL);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected.");
            mqtt_client = NULL;
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Topic: %.*s | Msg: %.*s",
                     event->topic_len, event->topic,
                     event->data_len,  event->data);

            if (strncmp(event->topic, TOPIC_LED_CONTROL,
                        event->topic_len) == 0)
            {
                handle_led(event->data, event->data_len);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error!");
            break;

        default:
            break;
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
    };
    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

static void heartbeat_task(void* arg)
{
    int uptime = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uptime += 10;
        if (mqtt_client != NULL) {
            char msg[48];
            snprintf(msg, sizeof(msg), "ONLINE|Up:%ds|LED:%s",
                     uptime, led_state ? "ON" : "OFF");
            publish_status(TOPIC_SYSTEM_STATUS, msg);
        }
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t base,
                                int32_t event_id, void* event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            wifi_retry++;
            ESP_LOGW(TAG, "Wi-Fi retry %d/%d", wifi_retry, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        wifi_retry = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected!");
    } else {
        ESP_LOGE(TAG, "Wi-Fi failed!");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
}

void app_main(void)
{
    /* Disable brownout detector */
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    printf("\n============================================\n");
    printf("  MQTT Home Automation - Altrobyte\n");
    printf("  Instructor: Pawan Meena\n");
    printf("============================================\n\n");

    gpio_init_pins();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    mqtt_start();

    xTaskCreate(heartbeat_task, "heartbeat", 4096, NULL, 5, NULL);
}