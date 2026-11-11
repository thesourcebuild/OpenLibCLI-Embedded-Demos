#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_credentials.h"
#include "cli_demo_setup.h"

#define CLI_TELNET_PORT 2323

static SemaphoreHandle_t s_ip_obtained = NULL;

static void on_got_ip(void *arg, esp_event_base_t base,
                      int32_t id, void *data)
{
  (void)arg;
  (void)base;
  (void)id;
  ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
  char ip[16];
  esp_ip4addr_ntoa(&evt->ip_info.ip, ip, sizeof(ip));
  printf("\r\n\r\nTelnet server listening on %s:%d\r\n\r\n", ip, CLI_TELNET_PORT);
  xSemaphoreGive(s_ip_obtained);
}

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
  (void)arg;
  (void)data;
  if (id == WIFI_EVENT_STA_START)
  {
    esp_wifi_connect();
  }
  else if (id == WIFI_EVENT_STA_DISCONNECTED)
  {
    esp_wifi_connect();
  }
}

void app_main(void)
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                      IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL));

  wifi_config_t wifi_config = {
      .sta = {
          .ssid = CLI_WIFI_SSID,
          .password = CLI_WIFI_PASSWORD,
      },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  s_ip_obtained = xSemaphoreCreateBinary();
  ESP_ERROR_CHECK(esp_wifi_start());

  xSemaphoreTake(s_ip_obtained, portMAX_DELAY);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(CLI_TELNET_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  bool listening = (listen_fd >= 0);

  if (listening)
  {
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    listening = (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
                 listen(listen_fd, 1) == 0);
    if (!listening)
    {
      close(listen_fd);
    }
  }
  if (!listening)
  {
    printf("Telnet server could not listen on port %d\r\n", CLI_TELNET_PORT);
  }

  /* One client at a time; when its session ends (disconnect) the next client is accepted. */
  while (listening)
  {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

    if (client_fd < 0)
    {
      vTaskDelay(pdMS_TO_TICKS(100)); /* back off: persistent errors would spin */
    }
    else
    {
      cli_demo_setup(client_fd);
      while (cli_demo_is_running() != 0U)
      {
        cli_demo_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
  }
}
