#ifndef IMUDRIVER_H
#define IMUDRIVER_H

#include <stdint.h>

typedef struct {
    uint8_t node_id;
    uint8_t tap_event;
    uint8_t imu_fault;
    uint8_t reserved;
} CompactTapData;

void imu_init(uint8_t node_id);

#endif
