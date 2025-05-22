/*******************************************************************************
* File Name: main.c
*
* Description:
*   Sanity‐check for UART on CY8CKIT-062S2-43012.
*   Blinks the user LED and prints “Hello” every 500 ms.
*******************************************************************************/

#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"
#include <stdio.h>

int main(void)
{
    cy_rslt_t r;

    /* 1) Initialize board and UART (115200) */
    r = cybsp_init();
    if (r != CY_RSLT_SUCCESS) { for (;;) {} }
    r = cy_retarget_io_init(CYBSP_DEBUG_UART_TX,
                            CYBSP_DEBUG_UART_RX,
                            CY_RETARGET_IO_BAUDRATE);
    if (r != CY_RSLT_SUCCESS) { for (;;) {} }

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\r\n*** UART & LED sanity test ***\r\n");

    /* 2) Initialize user LED */
    r = cyhal_gpio_init(CYBSP_USER_LED,
                        CYHAL_GPIO_DIR_OUTPUT,
                        CYHAL_GPIO_DRIVE_STRONG,
                        1);
    if (r != CY_RSLT_SUCCESS) { for (;;) {} }

    /* 3) Main loop: blink & print */
    for (;;)
    {
        cyhal_gpio_toggle(CYBSP_USER_LED);
        printf("Hello @ %lu ms\r\n", (unsigned long)cyhal_system_get_tick());
        cyhal_system_delay_ms(500);
    }
}
