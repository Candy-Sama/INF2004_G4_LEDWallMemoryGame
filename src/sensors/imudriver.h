#ifndef IMUDRIVER_H
#define IMUDRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "hardware/i2c.h"

typedef struct {
    int16_t x_mg;
    int16_t y_mg;
    int16_t z_mg;
} imu_acceleration_t;

typedef enum {
    IMU_READ_ERROR = -1,
    IMU_NO_NEW_DATA = 0,
    IMU_NEW_DATA = 1
} imu_read_result_t;

// The caller must initialise the I2C bus and GPIO pins first.
bool imu_init(i2c_inst_t *bus);
imu_read_result_t imu_read_acceleration(imu_acceleration_t *sample);

#endif
