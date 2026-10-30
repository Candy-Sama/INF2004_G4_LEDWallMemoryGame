#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

// Change this to the number of the board you are about to flash.
//#define NODE_ID 2

// Destination for node 1's movement commands.
//#define PEER_NODE_ID 2

// Desk movement demo: only node 1 sends commands to PEER_NODE_ID.
#define ENABLE_MOTION_TEST 1
#define IMU_LOG_READINGS 0  // Set to 1 to print X/Y/Z twice a second.
#define MOTION_THRESHOLD_MG 250
#define MOTION_RELEASE_MG 80
#define MOTION_QUIET_MS 200
#define MOTION_COOLDOWN_MS 800
 
// Orientation (accelerometer gravity vector, see src/sensors/orientation.h)
#define ENABLE_AUTO_ROTATE 1        // 1 = rotate card images to stay upright
#define ORIENT_SENSOR_OFFSET 0      // 0..3 clockwise quarter turns if the GY-511 is
                                    // mounted rotated relative to the LED panel
#define ORIENT_SETTLE_MS 300        // must hold a new pose this long to be accepted
#define ORIENT_MIN_MG 700           // ignore samples while moving (|a| outside 0.7-1.3 g)
#define ORIENT_MAX_MG 1300
#define ORIENT_FLAT_ENTER_MG 850    // |Z| above this = lying flat
#define ORIENT_FLAT_EXIT_MG 650     // |Z| must fall below this to leave "flat"
#define ORIENT_INPLANE_MIN_MG 300   // minimum X/Y gravity needed to pick a side
#define ORIENT_AXIS_HYST_MG 150     // extra margin before switching X<->Y dominance
#define ENABLE_ORIENTATION_SYNC 1   // 1 = rotating one panel rotates every panel (via MQTT)


/* 
#if NODE_ID < 1 || NODE_ID > 6
#error "NODE_ID must be between 1 and 6"
#endif 

// Two steps expand a number macro before converting it to text.
#define NODE_STRING_IMPL(value) #value
#define NODE_STRING(value) NODE_STRING_IMPL(value)

#define MQTT_STATUS_TOPIC "se4/memorygame/node" NODE_STRING(NODE_ID)
#define MQTT_COMMAND_TOPIC MQTT_STATUS_TOPIC "/command"
#define MQTT_PRESENCE_TOPIC MQTT_STATUS_TOPIC "/status"
#define MQTT_STATUS_MESSAGE "online"
#define MQTT_PEER_COMMAND_TOPIC "se4/memorygame/node" NODE_STRING(PEER_NODE_ID) "/command"
*/

// Topics are built at runtime from the panel number (node_identity.c):
#define MQTT_TOPIC_ROOT "se4/memorygame"
#define MQTT_STATUS_MESSAGE "online"

#define MQTT_ORIENTATION_TOPIC MQTT_TOPIC_ROOT "/orientation"

#define MQTT_JOIN_TOPIC        MQTT_TOPIC_ROOT "/join"

#endif