#pragma once

// THE ONE DOOR TO THE VEHICLE BUS. Every transmit goes through canTransmit();
// nothing else in src/ may call twai_transmit() (tests/test_tx_gate.py checks
// that). The gate is canTxGate() in powerpolicy.h: under POWER_IGNITION
// nothing transmits while the ignition is off.

#include <driver/twai.h>

bool canTxAllowed();

// twai_transmit() if the gate is open; ESP_ERR_INVALID_STATE (and a counted,
// rate-limited log line) if it is not.
esp_err_t canTransmit(const twai_message_t *msg, TickType_t ticksToWait);

uint32_t canTxBlocked();   // transmits refused by the gate since boot
uint32_t canTxFailed();    // gate open, driver refused (queue full, bus-off)
