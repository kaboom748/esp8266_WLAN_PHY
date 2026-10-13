#define FAST_ROLE 2
#define FAST_FS 312500
#define FAST_HALF_US 4500
#define FAST_BENCH 0
#define FAST_BPS 1
#define FAST_SPS 2
#define FAST_IQ 0
#define FAST_FAULT 0
#define FAST_MATCH 0
#define FAST_GAIN 0
#define FAST_BB_GAIN 0
#define FAST_INTEGER 2
#define FAST_CHANNEL 3
#define FAST_TX_CHANNEL 11
#define FAST_SYNC_LIMIT 4
#define FAST_ACCUM 63
#define FAST_SCAN 0
#define FAST_PHASE_DIVERSITY 0
#define FAST_QUIET_SDK 0
#define FAST_END_REPORT 0
#define FAST_DURATION 40
#include "legacy.h"
#include "fast.h"
FastModem application;
void setup() { application.begin(); }
void loop() { for(;;)application.poll(); }
