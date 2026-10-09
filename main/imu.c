/*
 * imu.c -- see imu.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "imu.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

/* src: original/compass.cpp's note, from m5tab5_esp_idf_m135_examples
 * tab5_bus.h: 0x68 is the Tab5's own BMI270, 0x69 the M135's. */
#define IMU_ADDR            (0x68)
/* src: original/compass.cpp I2C_FREQ, matched there to M5GFX's bus. */
#define IMU_HZ              (400000)
#define IMU_TIMEOUT_MS      (50)

/* src: BMI270_SensorAPI bmi2_defs.h and bmi270.h at the commit
 * tools/fetch_bmi270_config.py pins. */
#define REG_CHIP_ID         (0x00)
#define REG_ACC_X_LSB       (0x0C)
#define REG_INTERNAL_STATUS (0x21)
#define REG_ACC_CONF        (0x40)
#define REG_ACC_RANGE       (0x41)
#define REG_INIT_CTRL       (0x59)
#define REG_INIT_ADDR_0     (0x5B)
#define REG_INIT_DATA       (0x5E)
#define REG_PWR_CONF        (0x7C)
#define REG_PWR_CTRL        (0x7D)
#define REG_CMD             (0x7E)
#define CHIP_ID_BMI270      (0x24)
#define CMD_SOFT_RESET      (0xB6)
#define PWR_CTRL_ACC_EN     (0x04)      /* BMI2_ACC_EN_MASK */
#define STATUS_MASK         (0x0F)      /* BMI2_CONFIG_LOAD_STATUS_MASK */
#define STATUS_INIT_OK      (0x01)      /* BMI2_CONFIG_LOAD_SUCCESS */
#define IMAGE_LEN           (8192)
/* src: bmi2_soft_reset()'s 2000 us, BMI2_POWER_SAVE_MODE_DELAY_IN_US,
 * BMI2_INTERNAL_STATUS_READ_DELAY_MS (20000 us, named in ms there). */
#define RESET_US            (2000)
#define POWER_SAVE_US       (450)
#define STATUS_WAIT_MS      (20)
/* ACC_CONF: odr 50 Hz (BMI2_ACC_ODR_50HZ 0x07), bwp OSR4_AVG1 (0), power
 * optimised (BMI2_POWER_OPT_MODE 0) -- original/compass.cpp's choice:
 * which way is down, sampled at 10 Hz, a tremor smoothed out. ACC_RANGE
 * +-2 g (BMI2_ACC_RANGE_2G 0): finer counts on the only signal used. */
#define ACC_CONF_VALUE      (0x07)
#define ACC_RANGE_2G        (0x00)
/* Bytes of the image a burst carries. Bosch's must be even (word
 * addressed); the original ran its driver at 32. src: chosen, 64, with
 * 8192 a multiple of it. */
#define CHUNK               (64)

static i2c_master_dev_handle_t s_dev;
static bool s_ok;

static bool wr(uint8_t reg, const uint8_t *data, size_t n)
{
    static uint8_t buf[1 + CHUNK];      /* not on the stack (CLAUDE.md) */
    if (n > CHUNK) return false;
    buf[0] = reg;
    memcpy(buf + 1, data, n);
    return i2c_master_transmit(s_dev, buf, n + 1, IMU_TIMEOUT_MS) == ESP_OK;
}

static bool wr1(uint8_t reg, uint8_t v) { return wr(reg, &v, 1); }

static bool rd(uint8_t reg, uint8_t *data, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, n, IMU_TIMEOUT_MS) == ESP_OK;
}

bool imu_begin(i2c_master_bus_handle_t bus, const uint8_t *image, size_t len)
{
    s_ok = false;
    if (len != IMAGE_LEN) {
        ESP_LOGW(TAG, "no BMI270 configuration image in this build (%u bytes); "
                      "no accelerometer", (unsigned)len);
        return false;
    }
    if (i2c_master_probe(bus, IMU_ADDR, IMU_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "nothing at 0x%02X: no accelerometer", IMU_ADDR);
        return false;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMU_ADDR,
        .scl_speed_hz = IMU_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "could not add 0x%02X to the bus", IMU_ADDR);
        return false;
    }

    /* bmi2_sec_init(): a read to settle the interface, the ID, a reset. */
    uint8_t id = 0;
    rd(REG_CHIP_ID, &id, 1);
    if (!rd(REG_CHIP_ID, &id, 1) || id != CHIP_ID_BMI270) {
        ESP_LOGW(TAG, "0x%02X is not a BMI270 (chip id 0x%02X)", IMU_ADDR, id);
        goto fail;
    }
    if (!wr1(REG_CMD, CMD_SOFT_RESET)) goto fail;
    esp_rom_delay_us(RESET_US);
    rd(REG_CHIP_ID, &id, 1);    /* the interface again, after the reset */

    /* bmi2_write_config_file(): power save off, load off, the image a
     * burst at a time at its word address, load on, then the part's own
     * verdict -- the driver's OK is not the image having taken. */
    if (!wr1(REG_PWR_CONF, 0x00)) goto fail;
    esp_rom_delay_us(POWER_SAVE_US);
    if (!wr1(REG_INIT_CTRL, 0x00)) goto fail;
    for (size_t i = 0; i < len; i += CHUNK) {
        const uint8_t at[2] = { (uint8_t)((i / 2) & 0x0F), (uint8_t)((i / 2) >> 4) };
        if (!wr(REG_INIT_ADDR_0, at, 2) || !wr(REG_INIT_DATA, image + i, CHUNK)) {
            ESP_LOGW(TAG, "configuration upload failed at byte %u", (unsigned)i);
            goto fail;
        }
    }
    if (!wr1(REG_INIT_CTRL, 0x01)) goto fail;
    vTaskDelay(pdMS_TO_TICKS(STATUS_WAIT_MS) + 1);
    uint8_t st = 0;
    if (!rd(REG_INTERNAL_STATUS, &st, 1) || (st & STATUS_MASK) != STATUS_INIT_OK) {
        ESP_LOGW(TAG, "configuration rejected: INTERNAL_STATUS 0x%02X", st);
        goto fail;
    }

    /* The accelerometer alone. Power save stays off: the part is awake
     * for the accelerometer anyway, and with it on every access wants
     * 450 us around it. */
    if (!wr1(REG_ACC_CONF, ACC_CONF_VALUE) || !wr1(REG_ACC_RANGE, ACC_RANGE_2G) ||
        !wr1(REG_PWR_CTRL, PWR_CTRL_ACC_EN)) {
        ESP_LOGW(TAG, "accelerometer would not start");
        goto fail;
    }
    s_ok = true;
    ESP_LOGI(TAG, "BMI270 at 0x%02X: configured, accelerometer at 50 Hz, +-2 g", IMU_ADDR);
    return true;

fail:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

bool imu_read(int16_t *ax, int16_t *ay, int16_t *az)
{
    if (!s_ok) return false;
    /* src: BMI270 datasheet DATA_8..DATA_13, X, Y, Z little-endian. */
    uint8_t b[6];
    if (!rd(REG_ACC_X_LSB, b, sizeof(b))) return false;
    *ax = (int16_t)(b[1] << 8 | b[0]);
    *ay = (int16_t)(b[3] << 8 | b[2]);
    *az = (int16_t)(b[5] << 8 | b[4]);
    return true;
}
