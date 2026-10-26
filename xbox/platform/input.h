// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
int bbport_uwp_gamepad_poll(uint32_t *out_buttons, uint8_t out_axes[6],
                            int *out_touch_side, uint8_t *out_connected_count);
void bbport_uwp_gamepad_set_vibration(uint8_t large_motor, uint8_t small_motor);
const char *bbport_uwp_gamepad_name(void);
#ifdef __cplusplus
}
#endif
