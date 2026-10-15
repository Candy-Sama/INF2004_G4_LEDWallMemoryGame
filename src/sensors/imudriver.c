#include "imudriver.h"
#include <stdio.h>

// LSM303DLHC datasheet: https://www.st.com/resource/en/datasheet/lsm303dlhc.pdf
// Pico SDK uses seven-bit I2C addresses (not the shifted 0x32/0x33 forms).
#define ACCEL_ADDRESS 0x19
#define CTRL_REG1_A 0x20
#define CTRL_REG4_A 0x23
#define STATUS_REG_A 0x27
#define OUT_X_L_A 0x28
#define I2C_TIMEOUT_US 3000

static i2c_inst_t *imu_bus;
static bool imu_ready;

static bool read_registers(uint8_t reg, uint8_t *data, size_t length) {
    // Bit 7 enables address auto-increment for an accelerometer burst read.
    uint8_t subaddress = length > 1 ? (reg | 0x80) : reg;
    int result = i2c_write_timeout_us(imu_bus, ACCEL_ADDRESS, &subaddress,
                                    1, true, I2C_TIMEOUT_US);
    if (result != 1) {
        printf("IMU: register 0x%02X address write failed (%d).\n", reg, result);
        return false;
    }
    result = i2c_read_timeout_us(imu_bus, ACCEL_ADDRESS, data, length,
                                 false, I2C_TIMEOUT_US);
    if (result != (int)length) {
        printf("IMU: register 0x%02X read failed (%d).\n", reg, result);
        return false;
    }
    return true;
}

static bool write_register(uint8_t reg, uint8_t value) {
    uint8_t bytes[] = {reg, value};
    int result = i2c_write_timeout_us(imu_bus, ACCEL_ADDRESS, bytes,
                                    sizeof(bytes), false, I2C_TIMEOUT_US);
    if (result != (int)sizeof(bytes)) {
        printf("IMU: register 0x%02X write failed (%d).\n", reg, result);
        return false;
    }
    return true;
}

bool imu_init(i2c_inst_t *bus) {
    imu_bus = bus;
    imu_ready = false;
    if (bus == NULL) {
        return false;
    }
    uint8_t control;
    // Probe a documented register before configuring the expected GY-511 chip.
    if (!read_registers(CTRL_REG1_A, &control, 1)) {
        printf("IMU: no usable accelerometer response at 0x19. Check SDA/SCL, power and GND.\n");
        return false;
    }
    // 0x88: block data update, little endian, +/-2g, high resolution.
    // 0x57: 100 Hz sampling, normal power, X/Y/Z enabled.
    if (!write_register(CTRL_REG4_A, 0x88) ||
        !write_register(CTRL_REG1_A, 0x57)) {
        return false;
    }
    uint8_t control4;
    if (!read_registers(CTRL_REG1_A, &control, 1) ||
        !read_registers(CTRL_REG4_A, &control4, 1)) {
        return false;
    }
    if (control != 0x57 || control4 != 0x88) {
        printf("IMU: unexpected config readback: CTRL1=0x%02X CTRL4=0x%02X.\n",
               control, control4);
        return false;
    }
    imu_ready = true;
    printf("IMU: accelerometer at 0x19 configured for 100 Hz, +/-2g.\n");
    return true;
}

static int16_t decode_axis_mg(const uint8_t *bytes) {
    // Signed, little-endian, 12-bit sample left-aligned in 16 bits.
    // At +/-2g in high-resolution mode, one count is nominally 1 mg.
    uint16_t packed = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    int32_t signed_value = packed;
    if (packed & 0x8000) {
        signed_value -= 65536;
    }
    return (int16_t)(signed_value / 16);
}

imu_read_result_t imu_read_acceleration(imu_acceleration_t *sample) {
    if (!imu_ready || sample == NULL) {
        return IMU_READ_ERROR;
    }
    uint8_t status;
    if (!read_registers(STATUS_REG_A, &status, 1)) {
        return IMU_READ_ERROR;
    }
    if (!(status & 0x08)) {
        return IMU_NO_NEW_DATA;
    }
    uint8_t bytes[6];
    if (!read_registers(OUT_X_L_A, bytes, sizeof(bytes))) {
        return IMU_READ_ERROR;
    }
    sample->x_mg = decode_axis_mg(bytes);
    sample->y_mg = decode_axis_mg(bytes + 2);
    sample->z_mg = decode_axis_mg(bytes + 4);
    return IMU_NEW_DATA;
}
