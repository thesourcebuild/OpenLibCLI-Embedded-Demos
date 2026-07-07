#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cli_demo_setup.h"

void app_main(void)
{
  cli_demo_setup();

  while (1) {
    cli_demo_poll();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
