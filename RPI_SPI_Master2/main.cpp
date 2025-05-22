/*******************************************************************************
* File Name:   main.c
*
* Description:
*   PSoC6 two-phase SPI-slave echo at 25 kHz using HAL.
*   Phase 1: recv 1 byte; Phase 2: send it back.
*   We skip printing 0x00 “dummy” frames.
*******************************************************************************/
#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"
#include <stdio.h>

/* SPI settings */
#define SPI_FREQ_HZ    (25000UL)  // 25 kHz
#define BITS_PER_FRAME (8U)

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
    uint32_t    cmd;  // HAL API uses uint32_t

    /* 1) Init board + UART */
    result = cybsp_init();    handle_error(result);
    result = cy_retarget_io_init(
        CYBSP_DEBUG_UART_TX, CYBSP_DEBUG_UART_RX, CY_RETARGET_IO_BAUDRATE
    );                         handle_error(result);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\r\n*** 2-Phase HAL SPI Echo @25 kHz ***\r\n");

    /* 2) LED off */
    result = cyhal_gpio_init(
        CYBSP_USER_LED, CYHAL_GPIO_DIR_OUTPUT,
        CYHAL_GPIO_DRIVE_STRONG, 0
    );
    handle_error(result);

    /* 3) SPI slave init (MOSI, MISO, SCLK, CS=D10) */
    result = cyhal_spi_init(
        &spi,
        CYBSP_SPI_MOSI,
        CYBSP_SPI_MISO,
        CYBSP_SPI_CLK,
        CYBSP_SPI_CS,
        NULL,
        BITS_PER_FRAME,
        CYHAL_SPI_MODE_00_MSB,
        true
    );
    handle_error(result);

    /* 4) Set freq to 25 kHz */
    result = cyhal_spi_set_frequency(&spi, SPI_FREQ_HZ);
    handle_error(result);

    __enable_irq();

    /* 5) Main loop: Phase1 recv, Phase2 send, skip 0x00 prints */
    for (;;)
    {
        // Phase 1: receive one frame
        result = cyhal_spi_recv(&spi, &cmd);
        if (result != CY_RSLT_SUCCESS) {
            printf("SPI recv err: 0x%08lX\n", (unsigned long)result);
            continue;
        }
        uint8_t rxb = (uint8_t)cmd;
        if (rxb != 0x00) {
            printf("Phase1: Received 0x%02X\n", rxb);
        }

        // Drive LED on low bit
        cyhal_gpio_write(CYBSP_USER_LED, rxb & 1);

        // Phase 2: echo it back
        result = cyhal_spi_send(&spi, cmd);
        if (result != CY_RSLT_SUCCESS) {
            printf("SPI send err: 0x%08lX\n", (unsigned long)result);
            continue;
        }
        if (rxb != 0x00) {
            printf("Phase2: Echoed  0x%02X\n\n", rxb);
        }

        cyhal_system_delay_ms(50);
    }

    /* never reached */
    cyhal_spi_free(&spi);
    return 0;
}
