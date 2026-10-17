#define MF_ROLE 2
#define MF_PAYLOAD 192
#define MF_PARITY 32
#define MF_HALF_US 15000
#define MF_FS 625000
#define MF_SPS 2
#define MF_BPS 2
#define MF_TONE_STEP 1
#define MF_TONE_MAP 0
#define MF_RAW_SLICE 2
#define MF_UART_RF_GUARD 2
#define MF_RF_UART_PUMP 0
#define MF_CAPTURE_FAST 1
#define MF_BAUD 115200
#define MF_ACCUM 7
#define MF_GAIN 0
#define MF_BB_GAIN 0
#define MF_RX_CHANNEL 3
#define MF_TX_CHANNEL 11
#define MF_DROP_EVERY 0
#define MF_LOCKED_HYPOTHESES 1
#define MF_RAW_CANDIDATE_ORDER 1
#define MF_RTC_DIAGNOSTICS 0
#define MF_QUIET_SDK 1
#define MF_UART_TX_FIFO_LIMIT 8
#define MF_UART_RF_QUIET_CHARS 1
#define MF_UART_INPUT_RING_BYTES 2048
#define MF_UART_STOP_FREE 512
#define MF_UART_RESUME_FREE 1024
#define MF_COMPRESSION 1
#define MF_ADAPTIVE_FEC 1
#define MF_FAST_AFTER_ACKS 64
#define MF_DIAG_AFTER_MS 0
#define MF_RX_BLACKOUT_AT_MS 0
#define MF_RX_BLACKOUT_MS 0
#include "legacy.h"
#include "firmware.h"
FastBlockModem modem;
void setup(){modem.begin();}
void loop(){for(;;)modem.poll();}
