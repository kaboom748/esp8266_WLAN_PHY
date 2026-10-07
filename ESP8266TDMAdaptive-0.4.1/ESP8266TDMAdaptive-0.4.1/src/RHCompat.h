#ifndef RHCompat_h
#define RHCompat_h
#include <Arduino.h>
#define RH_BROADCAST_ADDRESS 0xff
#define RH_FLAGS_RESERVED 0xf0
#define RH_FLAGS_APPLICATION_SPECIFIC 0x0f
#define RH_FLAGS_NONE 0
#define RH_FLAGS_ACK 0x80
#define RH_FLAGS_RETRY 0x40
#define RH_DEFAULT_TIMEOUT 1500
#define RH_DEFAULT_RETRIES 3
#endif
