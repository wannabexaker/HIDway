#include "bootsel.h"

#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#define QSPI_SS_INDEX 1

#if PICO_RP2040
#define QSPI_CSN_IN_BIT (1u << 1)
#else
#define QSPI_CSN_IN_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif

bool __no_inline_not_in_flash_func(bootsel_pressed)(void)
{
    uint32_t irq = save_and_disable_interrupts();

    /* Float the chip-select so the button (to GND) can pull it low. */
    hw_write_masked(&ioqspi_hw->io[QSPI_SS_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    for (volatile int i = 0; i < 1000; ++i)
        ; /* let the line settle; no flash access is allowed here */

    bool pressed = !(sio_hw->gpio_hi_in & QSPI_CSN_IN_BIT);

    hw_write_masked(&ioqspi_hw->io[QSPI_SS_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(irq);
    return pressed;
}
