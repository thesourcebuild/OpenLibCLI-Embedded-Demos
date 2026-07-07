#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/stdio_usb.h"
#include "lwip/tcp.h"
#include "cli_demo_setup.h"
#include "wifi_credentials.h"

#define CLI_TELNET_PORT 2323

static struct tcp_pcb *s_server_pcb = NULL;
static struct tcp_pcb *s_client_pcb = NULL;

static err_t accept_cb(void *arg, struct tcp_pcb *client_pcb, err_t err)
{
  (void)arg;
  (void)err;
  s_client_pcb = client_pcb;
  printf("Client connected\n");
  /* Stop listening once we have a client */
  tcp_close(s_server_pcb);
  s_server_pcb = NULL;
  /* The transport will set up recv/err callbacks in cli_demo_setup */
  return ERR_OK;
}

int main(void)
{
  stdio_init_all();

  /* Wait up to 3s for USB CDC enumeration so debug prints are visible */
  for (int i = 0; i < 300 && !stdio_usb_connected(); i++)
  {
    sleep_ms(10);
  }

  if (cyw43_arch_init())
  {
    printf("cyw43_arch_init failed\n");
    return -1;
  }
  cyw43_arch_enable_sta_mode();

  for (;;)
  {
    printf("Connecting to %s ...\n", CLI_WIFI_SSID);
    int conn = cyw43_arch_wifi_connect_timeout_ms(CLI_WIFI_SSID, CLI_WIFI_PASSWORD,
                                                  CYW43_AUTH_WPA2_AES_PSK, 20000);
    if (conn == 0)
      break;
    printf("WiFi connection failed: %d, retrying in 20s\n", conn);
  }
  printf("WiFi connected\n");

  struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
  const char *ip = ip4addr_ntoa(netif_ip4_addr(netif));

  /* Create TCP listen PCB */
  s_server_pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
  if (!s_server_pcb)
  {
    printf("tcp_new failed\n");
    cyw43_arch_deinit();
    return -1;
  }

  err_t err = tcp_bind(s_server_pcb, IP_ANY_TYPE, CLI_TELNET_PORT);
  if (err != ERR_OK)
  {
    printf("tcp_bind failed: %d\n", err);
    tcp_close(s_server_pcb);
    cyw43_arch_deinit();
    return -1;
  }

  s_server_pcb = tcp_listen_with_backlog(s_server_pcb, 1);
  if (!s_server_pcb)
  {
    printf("tcp_listen failed\n");
    cyw43_arch_deinit();
    return -1;
  }
  tcp_accept(s_server_pcb, accept_cb);

  printf("Telnet server listening on %s:%u\n", ip, CLI_TELNET_PORT);

  /* Wait for a client */
  while (!s_client_pcb)
  {
    cyw43_arch_poll();
    sleep_ms(10);
  }

  cli_demo_setup(s_client_pcb);

  while (true)
  {
    cyw43_arch_poll();
    cli_demo_poll();
    sleep_ms(10);
  }

  cli_demo_shutdown();
  cyw43_arch_deinit();
  return 0;
}
