/*
 * imu.h -- the Tab5's own BMI270, its accelerometer alone, for
 * handling.c (0032).
 *
 * The Tab5's, not the M135's: with the magnetometer gone there is no
 * frame to share with it, and the board's own part is there with or
 * without a module, on the internal bus with no EXT5V to switch.
 *
 * Bring-up is Bosch's sequence (BMI270_SensorAPI bmi2_sec_init(),
 * bmi2_write_config_file(), set_accel_config()) by hand, accelerometer
 * only: the original used the whole driver because its magnetometer sat
 * behind the BMI270's auxiliary I2C master, which is what was hard to
 * drive. The 8 KB configuration image the part needs comes from
 * tools/fetch_bmi270_config.py, embedded at build time.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Find the BMI270 on `bus`, upload `image` (`len` bytes, 8192; 0 if the
 * build has none) and start the accelerometer at 50 Hz, +-2 g. False,
 * with a log line saying which step, if any of it fails; the map then
 * runs without, as it did before 0032.
 */
bool imu_begin(i2c_master_bus_handle_t bus, const uint8_t *image, size_t len);

/* The latest reading, raw counts. False if there is no IMU or the read
 * failed. */
bool imu_read(int16_t *ax, int16_t *ay, int16_t *az);

#ifdef __cplusplus
}
#endif
