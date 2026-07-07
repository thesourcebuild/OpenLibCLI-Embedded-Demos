#include <stdio.h>
#include "pico/stdlib.h"
#include "cli_demo_setup.h"

int main()
{
    stdio_init_all();

    cli_demo_setup();

    while (true)
    {
        cli_demo_poll();
    }
}
