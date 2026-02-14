#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "mdns.h"
#include <string.h>

#define LED_PIN    GPIO_NUM_2
#define WIFI_SSID  CONFIG_WIFI_SSID
#define WIFI_PASS  CONFIG_WIFI_PASSWORD

static bool led_state = false;

static void set_led(bool on)
{
    led_state = on;
    gpio_set_level(LED_PIN, on ? 1 : 0);
}

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

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI("wifi", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
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
}
