#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/stdio_usb.h"
#include "lwip/tcp.h"
#include "cli_demo_setup.h"
#include "wifi_credentials.h"

#define CLI_TELNET_PORT 2323

static struct tcp_pcb *s_server_pcb = NULL;

static err_t accept_cb(void *arg, struct tcp_pcb *client_pcb, err_t err)
{
  err_t result = ERR_OK;

  (void)arg;
  if (err != ERR_OK || client_pcb == NULL)
  {
    result = ERR_VAL;
  }
  else if (!cli_demo_accept(client_pcb))
  {
    /* One client at a time: the server keeps listening for the next one. */
    tcp_abort(client_pcb);
    result = ERR_ABRT;
  }
  else
  {
    /* The connection's callbacks are registered: the main loop starts the session. */
    printf("Client connected\n");
  }

  return result;
}

/* Join the WiFi network, retrying until it succeeds. */
static void wifi_connect(void)
{
  int conn = -1;

  while (conn != 0)
  {
    printf("Connecting to %s ...\n", CLI_WIFI_SSID);
    /* Waits up to 20 s per attempt (until an address is assigned). */
    conn = cyw43_arch_wifi_connect_timeout_ms(CLI_WIFI_SSID, CLI_WIFI_PASSWORD,
                                              CYW43_AUTH_WPA2_AES_PSK, 20000);
    if (conn != 0)
    {
      printf("WiFi connection failed: %d, retrying\n", conn);
    }
  }
  printf("WiFi connected: %s\n", ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
}

/* Join the network again when the device was dropped from it. After a lost signal the WiFi
 * chip rejoins by itself (CYW43_LINK_JOIN, then NOIP until DHCP is done); after being
 * disconnected by the access point, or a failed attempt, nothing rejoins on its own. */
static void wifi_keep_connected(void)
{
  if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) <= CYW43_LINK_DOWN)
  {
    printf("WiFi connection lost\n");
    wifi_connect();
  }
}

int main(void)
{
  int result = -1; /* main() only returns when the server could not start */
  bool listening = false;

  stdio_init_all();

  /* Wait up to 3s for USB CDC enumeration so debug prints are visible */
  for (int i = 0; i < 300 && !stdio_usb_connected(); i++)
  {
    sleep_ms(10);
  }

  if (cyw43_arch_init())
  {
    printf("cyw43_arch_init failed\n");
  }
  else
  {
    cyw43_arch_enable_sta_mode();
    wifi_connect();

    /* Create the TCP listen PCB */
    cyw43_arch_lwip_begin();
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL)
    {
      printf("tcp_new failed\n");
    }
    else
    {
      err_t err = tcp_bind(pcb, IP_ANY_TYPE, CLI_TELNET_PORT);
      if (err != ERR_OK)
      {
        printf("tcp_bind failed: %d\n", err);
        tcp_close(pcb);
      }
      else
      {
        s_server_pcb = tcp_listen_with_backlog(pcb, 1);
        if (s_server_pcb == NULL)
        {
          printf("tcp_listen failed\n");
          tcp_close(pcb); /* not freed by a failed tcp_listen() */
        }
        else
        {
          tcp_accept(s_server_pcb, accept_cb);
          listening = true;
        }
      }
    }
    cyw43_arch_lwip_end();

    if (listening)
    {
      printf("Telnet server listening on port %u\n", CLI_TELNET_PORT);
    }

    /* One session at a time; when it ends the next client is accepted. The listening PCB
     * is bound to any address, so it keeps working across a rejoin (also with a new IP). */
    while (listening)
    {
      wifi_keep_connected();
      if (cli_demo_client_waiting())
      {
        cli_demo_setup();
        while (cli_demo_is_running() != 0U)
        {
          /* A session whose client is unreachable after a rejoin ends through keepalive. */
          wifi_keep_connected();
          cyw43_arch_poll();
          cli_demo_poll();
          sleep_ms(10);
        }
        /* Closed by cli_demo_shutdown(), which also frees the slot for the next client. */
        printf("Client disconnected\n");
      }
      cyw43_arch_poll();
      sleep_ms(10);
    }

    cyw43_arch_deinit();
  }

  return result;
}
