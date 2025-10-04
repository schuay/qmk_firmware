#include "quantum.h"
#include "quantum/logging/print.h"
#include "quantum/encoder.h"
#include "wait.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

// This includes the PIO program array from your generated header
#include "pio_encoder.pio.h"

static PIO pio = pio1;
static int sm = -1;
static uint pio_offset = 0;

// A static variable to hold the latest count received from the PIO
static volatile int32_t pio_encoder_count = 0;

void pio_encoder_init(pin_t pin_base) {
    // This function should be called from your keyboard_post_init_user()
    uint pio_idx = pio_get_index(pio);
    hal_lld_peripheral_unreset(pio_idx == 0 ? RESETS_ALLREG_PIO0 : RESETS_ALLREG_PIO1);

    // --- PIO and State Machine Setup ---
    if (!pio_can_add_program(pio, &quadrature_encoder_program)) {
        uprintf("PIO Encoder: ERROR - Cannot add PIO program\n");
        return;
    }

    pio_offset = pio_add_program(pio, &quadrature_encoder_program);
    if (pio_offset != 0) {
        uprintf("PIO Encoder: ERROR - pio_offset must be 0\n");
        return;
    }

    sm = pio_claim_unused_sm(pio, true);
    if (sm < 0) {
        uprintf("PIO Encoder: ERROR - Cannot claim state machine\n");
        return;
    }

    iomode_t encoder_pin_mode = PAL_RP_PAD_PUE |     // Enable Pull-Up
                                PAL_RP_PAD_SCHMITT | // Enable Schmitt trigger for cleaner edges
                                PAL_RP_PAD_IE |      // Enable Input
                                (pio_idx == 0 ? PAL_MODE_ALTERNATE_PIO0 : PAL_MODE_ALTERNATE_PIO1);
    palSetLineMode(pin_base, encoder_pin_mode);
    palSetLineMode(pin_base + 1, encoder_pin_mode);

    dprintf("offset %d sm %d\n", (int)pio_offset, (int)sm);

    dprintf("%d %d\n", (int)pin_base, (int)GP6);
    pin_base = GP6;
    quadrature_encoder_program_init(pio, sm, pin_base, 0, pio_offset);

    dprintf("PIO Encoder: Initialized on PIO1, SM %d\n", sm);
}

// This function should be called from your main keyboard task loop
void pio_encoder_task(void) {
    if (sm < 0) return;

    // Drain the FIFO of all pending counts, updating our static variable
    // with the latest value.
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        pio_encoder_count = pio_sm_get(pio, sm);
    }
}

int32_t quadrature_encoder_get_count2(void) {
    return quadrature_encoder_get_count(pio, sm);
}
