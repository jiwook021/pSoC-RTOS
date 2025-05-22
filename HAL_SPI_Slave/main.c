/*******************************************************************************
* File Name:   main.c
*
* Description:
*   PSoC6 SPI‐slave that receives a single byte over SPI at 25 kHz
*   and drives the LED based on the low bit of the received byte.
*******************************************************************************/
#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"
#include <stdio.h>

/* SPI settings */
#define SPI_FREQ_HZ      (25000UL)   // 25 kHz
#define BITS_PER_FRAME   (8U)

/* Error handler */
static void handle_error(cy_rslt_t status)
{
    if (status != CY_RSLT_SUCCESS) {
        printf("Fatal error: 0x%08lX\n", (unsigned long)status);
        CY_ASSERT(false);
    }
}

int main(void)
{
    cy_rslt_t   result;
    cyhal_spi_t spi;
    uint32_t    cmd;   // must be 32-bit

    /* 1) Board & UART init */
    result = cybsp_init();
    handle_error(result);

    result = cy_retarget_io_init(CYBSP_DEBUG_UART_TX,
                                 CYBSP_DEBUG_UART_RX,
                                 CY_RETARGET_IO_BAUDRATE);
    handle_error(result);

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\r\n*** HAL SPI Slave @ 25 kHz ***\r\n");

    /* 2) LED init */
    result = cyhal_gpio_init(CYBSP_USER_LED,
                             CYHAL_GPIO_DIR_OUTPUT,
                             CYHAL_GPIO_DRIVE_STRONG,
                             0);
    handle_error(result);

    /* 3) SPI slave init: MOSI, MISO, CLK, CS */
    result = cyhal_spi_init(&spi,
                            CYBSP_SPI_MOSI,
                            CYBSP_SPI_MISO,
                            CYBSP_SPI_CLK,
                            CYBSP_SPI_CS,
                            NULL,
                            BITS_PER_FRAME,
                            CYHAL_SPI_MODE_00_MSB,
                            true);
    handle_error(result);

    /* 4) Set the SPI frequency to 25 kHz */
    result = cyhal_spi_set_frequency(&spi, SPI_FREQ_HZ);
    handle_error(result);

    /* 5) Enable global IRQs */
    __enable_irq();

    /* 6) Main loop: blocking receive of one 8-bit frame (in a uint32_t) */
    for (;;)
    {
        result = cyhal_spi_recv(&spi, &cmd);
        if (result == CY_RSLT_SUCCESS) {
            uint8_t cb = (uint8_t)cmd;
            cyhal_gpio_write(CYBSP_USER_LED, cb & 1);
            printf("Received 0x%02X → LED = %d\n", cb, cb & 1);
        }
        else {
            printf("SPI recv error: 0x%08lX\n", (unsigned long)result);
        }
    }

    /* never reached */
    cyhal_spi_free(&spi);
    return 0;
}
