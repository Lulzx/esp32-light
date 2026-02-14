#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/touch_pad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "mdns.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

#define LED_PIN    GPIO_NUM_2
#define TOUCH_PAD  TOUCH_PAD_NUM0  // GPIO 4
#define TOUCH_THRESHOLD 400
#define WIFI_SSID  CONFIG_WIFI_SSID
#define WIFI_PASS  CONFIG_WIFI_PASSWORD
#define BOT_TOKEN  CONFIG_TELEGRAM_BOT_TOKEN

#define TAG "app"
#define TG_HOST "https://api.telegram.org/bot"

static bool led_state = false;
static bool wifi_connected = false;

static void set_led(bool on)
{
    led_state = on;
    gpio_set_level(LED_PIN, on ? 1 : 0);
}

/* ── Web UI ─────────────────────────────────────────────── */

static const char *HTML_PAGE =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Light</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:system-ui,-apple-system,sans-serif;display:flex;"
    "justify-content:center;align-items:center;min-height:100vh;"
    "background:#0a0a0f;color:#fff;overflow:hidden}"
    ".bg{position:fixed;top:0;left:0;width:100%%;height:100%%;"
    "background:radial-gradient(circle at 50%% 50%%,#12121a,#0a0a0f);z-index:0}"
    ".c{text-align:center;z-index:1;position:relative}"
    "h1{font-size:1.1em;font-weight:400;letter-spacing:.15em;"
    "text-transform:uppercase;color:#444;margin-bottom:3em}"
    ".ring{width:180px;height:180px;border-radius:50%%;display:flex;"
    "align-items:center;justify-content:center;margin:0 auto 2.5em;"
    "transition:all .4s cubic-bezier(.4,0,.2,1);cursor:pointer;"
    "position:relative;-webkit-tap-highlight-color:transparent}"
    ".ring.off{background:#141419;box-shadow:inset 0 2px 4px rgba(0,0,0,.5),"
    "0 0 0 1px rgba(255,255,255,.03)}"
    ".ring.on{background:linear-gradient(135deg,#2563eb,#3b82f6);"
    "box-shadow:0 0 60px rgba(59,130,246,.3),0 0 120px rgba(59,130,246,.1),"
    "inset 0 1px 0 rgba(255,255,255,.15)}"
    ".icon{width:48px;height:48px;transition:all .4s}"
    ".ring.off .icon{opacity:.2}"
    ".ring.on .icon{opacity:1;filter:drop-shadow(0 0 8px rgba(255,255,255,.5))}"
    ".label{font-size:.85em;font-weight:500;letter-spacing:.3em;"
    "text-transform:uppercase;transition:all .4s}"
    ".ring.off~.label{color:#333}"
    ".ring.on~.label{color:#3b82f6}"
    "</style></head><body>"
    "<div class='bg'></div>"
    "<div class='c'>"
    "<h1>ESP32 Light</h1>"
    "<div class='ring %s' id='r' onclick='t()'>"
    "<svg class='icon' viewBox='0 0 24 24' fill='none' stroke='white' stroke-width='1.5'>"
    "<path d='M18.36 6.64a9 9 0 1 1-12.73 0'/>"
    "<line x1='12' y1='2' x2='12' y2='12'/></svg></div>"
    "<div class='label' id='l'>%s</div>"
    "</div>"
    "<script>"
    "function t(){"
    "var r=document.getElementById('r'),l=document.getElementById('l');"
    "var on=r.classList.contains('off');"
    "r.className='ring '+(on?'on':'off');"
    "l.textContent=on?'on':'off';"
    "fetch('/toggle')}"
    "</script></body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    char buf[2200];
    const char *cls = led_state ? "on" : "off";
    const char *label = led_state ? "on" : "off";
    snprintf(buf, sizeof(buf), HTML_PAGE, cls, label);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, buf, strlen(buf));
}

static esp_err_t toggle_handler(httpd_req_t *req)
{
    set_led(!led_state);
    const char *resp = led_state ? "1" : "0";
    return httpd_resp_send(req, resp, 1);
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root   = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
        httpd_uri_t toggle = { .uri = "/toggle", .method = HTTP_GET, .handler = toggle_handler };
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &toggle);
    }
}

/* ── Telegram Bot ───────────────────────────────────────── */

typedef struct {
    char *buf;
    int len;
    int cap;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *b = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->len + evt->data_len < b->cap - 1) {
            memcpy(b->buf + b->len, evt->data, evt->data_len);
            b->len += evt->data_len;
            b->buf[b->len] = 0;
        }
    }
    return ESP_OK;
}

static int tg_request(const char *method, const char *params, char *resp, int resp_cap)
{
    char url[256];
    snprintf(url, sizeof(url), TG_HOST "%s/%s", BOT_TOKEN, method);

    http_buf_t hb = { .buf = resp, .len = 0, .cap = resp_cap };
    resp[0] = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &hb,
        .timeout_ms = 30000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    if (params && params[0]) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, params, strlen(params));
    }

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK ? 0 : -1;
}

static void tg_send(int64_t chat_id, const char *text)
{
    char params[256];
    char resp[256];
    snprintf(params, sizeof(params),
        "{\"chat_id\":%" PRId64 ",\"text\":\"%s\"}", chat_id, text);
    tg_request("sendMessage", params, resp, sizeof(resp));
}

static void handle_command(int64_t chat_id, const char *text)
{
    if (strcmp(text, "/on") == 0) {
        set_led(true);
        tg_send(chat_id, "Light is ON");
    } else if (strcmp(text, "/off") == 0) {
        set_led(false);
        tg_send(chat_id, "Light is OFF");
    } else if (strcmp(text, "/toggle") == 0) {
        set_led(!led_state);
        tg_send(chat_id, led_state ? "Light is ON" : "Light is OFF");
    } else if (strcmp(text, "/status") == 0) {
        tg_send(chat_id, led_state ? "Light is ON" : "Light is OFF");
    } else if (strcmp(text, "/start") == 0) {
        tg_send(chat_id, "Commands: /on /off /toggle /status");
    } else {
        tg_send(chat_id, "Unknown. Try /on /off /toggle /status");
    }
}

static void telegram_task(void *arg)
{
    char *resp = malloc(4096);
    if (!resp) vTaskDelete(NULL);

    int64_t offset = 0;

    while (!wifi_connected) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    ESP_LOGI(TAG, "Telegram bot started");

    while (1) {
        char params[128];
        snprintf(params, sizeof(params),
            "{\"offset\":%" PRId64 ",\"timeout\":20}", offset);

        if (tg_request("getUpdates", params, resp, 4096) != 0) {
            ESP_LOGW(TAG, "Telegram poll failed, retrying...");
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        cJSON *root = cJSON_Parse(resp);
        if (!root) {
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        cJSON *result = cJSON_GetObjectItem(root, "result");
        if (cJSON_IsArray(result)) {
            int count = cJSON_GetArraySize(result);
            for (int i = 0; i < count; i++) {
                cJSON *update = cJSON_GetArrayItem(result, i);
                cJSON *uid = cJSON_GetObjectItem(update, "update_id");
                if (uid) offset = uid->valuedouble + 1;

                cJSON *msg = cJSON_GetObjectItem(update, "message");
                if (!msg) continue;

                cJSON *chat = cJSON_GetObjectItem(msg, "chat");
                cJSON *text = cJSON_GetObjectItem(msg, "text");
                if (!chat || !text) continue;

                cJSON *cid = cJSON_GetObjectItem(chat, "id");
                if (!cid) continue;

                int64_t chat_id = (int64_t)cid->valuedouble;
                handle_command(chat_id, text->valuestring);
            }
        }

        cJSON_Delete(root);
    }
}

/* ── Touch Sensing ──────────────────────────────────────── */

static void touch_task(void *arg)
{
    touch_pad_init();
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_config(TOUCH_PAD, 0);
    touch_pad_filter_start(10);

    // Calibrate: read baseline value
    uint16_t baseline = 0;
    vTaskDelay(500 / portTICK_PERIOD_MS);
    touch_pad_read_filtered(TOUCH_PAD, &baseline);
    ESP_LOGI(TAG, "Touch baseline: %d (threshold: %d)", baseline, TOUCH_THRESHOLD);

    bool was_touched = false;

    while (1) {
        uint16_t val = 0;
        touch_pad_read_filtered(TOUCH_PAD, &val);

        // Touch lowers the value below threshold
        bool touched = val < TOUCH_THRESHOLD;

        if (touched && !was_touched) {
            set_led(!led_state);
            ESP_LOGI(TAG, "Touch! LED %s (val=%d)", led_state ? "ON" : "OFF", val);
        }
        was_touched = touched;

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

/* ── WiFi ───────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

/* ── Main ───────────────────────────────────────────────── */

void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    set_led(false);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    start_webserver();

    mdns_init();
    mdns_hostname_set("esp32");
    mdns_instance_name_set("ESP32 Light");
    mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, NULL, 0);

    xTaskCreate(telegram_task, "telegram", 8192, NULL, 5, NULL);
    xTaskCreate(touch_task, "touch", 2048, NULL, 10, NULL);
}
