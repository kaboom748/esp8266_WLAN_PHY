#pragma once
#include <stdint.h>
#include <stddef.h>
#ifndef MF_UART_HOST_TEST
#include <Arduino.h>
extern "C" {
#include <user_interface.h>
}
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
namespace UartLinkHost {
uint32_t now();
unsigned rxCount();
uint8_t readByte();
unsigned txCount();
void writeByte(uint8_t);
uint32_t errors();
void clearErrors();
void resetRx();
}
#endif
#ifndef MF_BAUD
#define MF_BAUD 115200
#endif
#ifndef MF_UART_RING_BYTES
#define MF_UART_RING_BYTES 2048
#endif
#ifndef MF_UART_INPUT_RING_BYTES
#define MF_UART_INPUT_RING_BYTES MF_UART_RING_BYTES
#endif
#ifndef MF_UART_STOP_FREE
#define MF_UART_STOP_FREE 512
#endif
#ifndef MF_UART_RESUME_FREE
#define MF_UART_RESUME_FREE 1024
#endif
#ifndef MF_UART_RF_GUARD
#define MF_UART_RF_GUARD 0
#endif
#ifndef MF_UART_TX_FIFO_LIMIT
#define MF_UART_TX_FIFO_LIMIT 8
#endif
#ifndef MF_UART_RF_QUIET_CHARS
#define MF_UART_RF_QUIET_CHARS 1
#endif

// Raw UART with standard software flow control. Only XON/XOFF are reserved.
// No framing, escaping, UART handshake, diagnostics or retransmission is used.
namespace UartLink {
// CAPACITY/MASK retain their existing output-ring meaning for callers.
constexpr unsigned CAPACITY=MF_UART_RING_BYTES,MASK=CAPACITY-1;
constexpr unsigned INPUT_CAPACITY=MF_UART_INPUT_RING_BYTES,INPUT_MASK=INPUT_CAPACITY-1;
constexpr unsigned STOP_FREE=MF_UART_STOP_FREE,RESUME_FREE=MF_UART_RESUME_FREE;
constexpr uint32_t FLOW_REPEAT_US=250000;
static_assert(CAPACITY>=1024 && CAPACITY<=32768 && !(CAPACITY&MASK),
              "UART output ring must be a power of two, 1024..32768");
static_assert(INPUT_CAPACITY>0 && INPUT_CAPACITY<=32768 && !(INPUT_CAPACITY&INPUT_MASK),
              "UART input ring must be a power of two, at most 32768");
static_assert(STOP_FREE>0 && STOP_FREE<RESUME_FREE && RESUME_FREE<=INPUT_CAPACITY,
              "UART flow thresholds must satisfy 0 < STOP_FREE < RESUME_FREE <= input capacity");
static_assert(MF_BAUD>0,"invalid UART baud rate");
static_assert(MF_UART_TX_FIFO_LIMIT>=1 && MF_UART_TX_FIFO_LIMIT<=120,"invalid UART TX FIFO limit");
static_assert(MF_UART_RF_QUIET_CHARS>=1 && MF_UART_RF_QUIET_CHARS<=16,"invalid UART RF quiet characters");
struct Stats {
  uint32_t rxBytes=0,txBytes=0,rxPayload=0,txPayload=0;
  uint32_t rxOverflow=0,rxHardwareErrors=0,rxBreaks=0,rxFrameErrors=0,rxParityErrors=0;
  // Retained for firmware source compatibility; no UART protocol is active.
  uint32_t rxProtocolErrors=0,rxEscapes=0,malformedControls=0,controlRequests=0;
  uint32_t txControls=0,txControlBusy=0;
  uint32_t sentXon=0,sentXoff=0,receivedXon=0,receivedXoff=0;
  uint32_t inputPeak=0,outputPeak=0,maxPumpGapUs=0;
  uint32_t guardCalls=0,guardFailures=0,guardMaxUs=0;
  uint32_t txRetries=0,rxCrcReject=0,txAckedBytes=0,rxDuplicates=0,rxFrameDrops=0,handshakeReject=0;
};
static Stats stats;
namespace Detail {
static uint8_t input[INPUT_CAPACITY],output[CAPACITY];
// Input: pump produces, foreground consumes. Output: foreground produces,
// pump consumes. Publish an index only after copying/writing its bytes.
static volatile uint16_t inputHead=0,inputTail=0,outputHead=0,outputTail=0;
static volatile bool linkReady=false,fault=false,peerStopped=false,localStopped=true;
static volatile bool txPausedForRf=false;
static bool flowDirty=true,haveLastPump=false;
static uint32_t lastFlow=0,lastPump=0;

static inline uint32_t IRAM_ATTR lock() {
#ifndef MF_UART_HOST_TEST
  uint32_t state;__asm__ volatile("rsil %0, 15":"=a"(state)::"memory");return state;
#else
  return 0;
#endif
}
static inline void IRAM_ATTR unlock(uint32_t state) {
#ifndef MF_UART_HOST_TEST
  __asm__ volatile("wsr %0, ps; rsync"::"a"(state):"memory");
#else
  (void)state;
#endif
}
static inline void IRAM_ATTR fence() {
#ifndef MF_UART_HOST_TEST
  __asm__ volatile("memw":::"memory");
#endif
}
static inline uint32_t IRAM_ATTR now() {
#ifndef MF_UART_HOST_TEST
  return micros();
#else
  return UartLinkHost::now();
#endif
}
static inline unsigned IRAM_ATTR rxCount() {
#ifndef MF_UART_HOST_TEST
  return U0S&255;
#else
  return UartLinkHost::rxCount();
#endif
}
static inline uint8_t IRAM_ATTR readByte() {
#ifndef MF_UART_HOST_TEST
  return uint8_t(U0F);
#else
  return UartLinkHost::readByte();
#endif
}
static inline unsigned IRAM_ATTR txCount() {
#ifndef MF_UART_HOST_TEST
  return (U0S>>16)&255;
#else
  return UartLinkHost::txCount();
#endif
}
static inline void IRAM_ATTR emit(uint8_t b) {
#ifndef MF_UART_HOST_TEST
  U0F=b;
#else
  UartLinkHost::writeByte(b);
#endif
  ++stats.txBytes;
}
static inline unsigned IRAM_ATTR inputUsed(){return uint16_t(inputHead-inputTail);}
static inline unsigned IRAM_ATTR outputUsed(){return uint16_t(outputHead-outputTail);}
static void IRAM_ATTR receiveByte(uint8_t b) {
  ++stats.rxBytes;
  if(b==0x11){peerStopped=false;++stats.receivedXon;return;}
  if(b==0x13){peerStopped=true;++stats.receivedXoff;return;}
  if(fault)return;
  const unsigned used=inputUsed();
  if(used==INPUT_CAPACITY){++stats.rxOverflow;fault=true;return;}
  const uint16_t head=inputHead;
  input[head&INPUT_MASK]=b;fence();inputHead=uint16_t(head+1);++stats.rxPayload;
  if(used+1>stats.inputPeak)stats.inputPeak=used+1;
}
static void IRAM_ATTR checkErrors() {
#ifndef MF_UART_HOST_TEST
  const uint32_t flags=U0IR&((1<<UIOF)|(1<<UIFR)|(1<<UIPE)|(1<<UIBD));
  if(!flags)return;
  if(flags&(1<<UIOF))++stats.rxOverflow;
  if(flags&(1<<UIFR))++stats.rxFrameErrors;
  if(flags&(1<<UIPE))++stats.rxParityErrors;
  if(flags&(1<<UIBD))++stats.rxBreaks;
  U0C0|=1<<UCRXRST;U0C0&=~(1<<UCRXRST);U0IC=flags;
#else
  const uint32_t flags=UartLinkHost::errors();if(!flags)return;
  if(flags&1)++stats.rxOverflow;
  if(flags&2)++stats.rxFrameErrors;
  if(flags&4)++stats.rxParityErrors;
  if(flags&8)++stats.rxBreaks;
  UartLinkHost::resetRx();UartLinkHost::clearErrors();
#endif
  // Raw UART cannot recover a missing/corrupt byte. Keep the admitted prefix,
  // latch the failure and assert XOFF instead of continuing a damaged stream.
  ++stats.rxHardwareErrors;fault=true;
}
static inline void IRAM_ATTR updateFlow(uint32_t at) {
  const unsigned available=INPUT_CAPACITY-inputUsed();bool stop=localStopped;
  if(!linkReady || fault || available<=STOP_FREE)stop=true;
  else if(available>=RESUME_FREE)stop=false;
  if(stop!=localStopped){localStopped=stop;flowDirty=true;}
  if(uint32_t(at-lastFlow)>=FLOW_REPEAT_US)flowDirty=true;
}
static bool IRAM_ATTR transmitByte(uint32_t at) {
  if((MF_UART_RF_GUARD && txPausedForRf) || txCount()>=MF_UART_TX_FIFO_LIMIT)return false;
  // Flow control must work even when the peer has stopped our payload TX.
  if(flowDirty) {
    emit(localStopped?0x13:0x11);
    if(localStopped)++stats.sentXoff;else ++stats.sentXon;
    flowDirty=false;lastFlow=at;return true;
  }
  if(peerStopped || fault || outputHead==outputTail)return false;
  const uint16_t tail=outputTail;
  emit(output[tail&MASK]);fence();outputTail=uint16_t(tail+1);
  // Counts FIFO admission, not receipt by the PC (there is no UART ACK).
  ++stats.txPayload;return true;
}
} // namespace Detail

// Bounded work under the interrupt mask. Foreground service drains larger
// batches by repeating this short pump with interrupts enabled between calls.
static void IRAM_ATTR pump() {
  const uint32_t state=Detail::lock(),at=Detail::now();
  if(Detail::haveLastPump){const uint32_t gap=at-Detail::lastPump;if(gap>stats.maxPumpGapUs)stats.maxPumpGapUs=gap;}
  Detail::lastPump=at;Detail::haveLastPump=true;Detail::checkErrors();
  unsigned available=Detail::rxCount();if(available>2)available=2;
  for(unsigned n=0;n<available;++n)Detail::receiveByte(Detail::readByte());
  Detail::updateFlow(at);
  for(unsigned n=0;n<2;++n)if(!Detail::transmitByte(at))break;
  Detail::unlock(state);
}
static unsigned inputSize(){return Detail::inputUsed();}
static unsigned outputFree(){return CAPACITY-Detail::outputUsed();}
static bool healthy(){return !Detail::fault;}
static bool peerPaused(){return Detail::peerStopped;}
static bool inputPaused(){return Detail::localStopped;}
static uint32_t sessionId(){return 0;}
static unsigned readInput(uint8_t* destination,unsigned maximum) {
  unsigned count=Detail::inputUsed();if(count>maximum)count=maximum;
  const uint16_t tail=Detail::inputTail;
  for(unsigned n=0;n<count;++n)destination[n]=Detail::input[(tail+n)&INPUT_MASK];
  Detail::fence();Detail::inputTail=uint16_t(tail+count);return count;
}
static bool writeOutput(const uint8_t* source,unsigned count) {
  const unsigned used=Detail::outputUsed();if(Detail::fault || count>CAPACITY-used)return false;
  const uint16_t head=Detail::outputHead;
  for(unsigned n=0;n<count;++n)Detail::output[(head+n)&MASK]=source[n];
  Detail::fence();Detail::outputHead=uint16_t(head+count);
  if(used+count>stats.outputPeak)stats.outputPeak=used+count;
  return true;
}
static bool takeStatsRequest(){return false;}
static bool takeClearRequest(){return false;}
static Stats snapshotStats(){const uint32_t s=Detail::lock();const Stats v=stats;Detail::unlock(s);return v;}
static void clearStats() {
  const uint32_t s=Detail::lock();stats=Stats{};stats.inputPeak=Detail::inputUsed();stats.outputPeak=Detail::outputUsed();
  Detail::haveLastPump=false;Detail::unlock(s);
}
// Compatibility only: consume the request without ever emitting diagnostics.
static bool sendControl(const char*){return true;}
static void setLinkReady(bool ready){Detail::linkReady=ready;}
static void service() {
  for(unsigned n=0;n<16;++n){pump();if(!Detail::rxCount())break;}
}
static bool pauseTxForRf() {
  if(!MF_UART_RF_GUARD)return true;
  const uint32_t state=Detail::lock();Detail::txPausedForRf=true;Detail::unlock(state);++stats.guardCalls;
  const uint32_t began=Detail::now();
  constexpr uint32_t QUIET_US=MF_UART_RF_QUIET_CHARS*((10000000UL+MF_BAUD-1)/MF_BAUD)+20;
  bool observedEmpty=false;uint32_t emptyAt=0;
  for(;;) {
    pump();const uint32_t at=Detail::now(),elapsed=at-began;
    if(Detail::txCount())observedEmpty=false;
    else if(!observedEmpty){observedEmpty=true;emptyAt=at;}
    else if(uint32_t(at-emptyAt)>=QUIET_US){if(elapsed>stats.guardMaxUs)stats.guardMaxUs=elapsed;return true;}
    if(elapsed>=2000){if(elapsed>stats.guardMaxUs)stats.guardMaxUs=elapsed;++stats.guardFailures;return false;}
  }
}
static void resumeTxAfterRf(){if(MF_UART_RF_GUARD)Detail::txPausedForRf=false;}
static void begin() {
#ifndef MF_UART_HOST_TEST
  Serial.setRxBufferSize(64);Serial.begin(MF_BAUD,SERIAL_8N1);Serial.setDebugOutput(false);system_set_os_print(0);
#endif
  const uint32_t state=Detail::lock();
#ifndef MF_UART_HOST_TEST
  U0IE=0;U0IC=0xffff;
#endif
  Detail::inputHead=Detail::inputTail=Detail::outputHead=Detail::outputTail=0;
  Detail::linkReady=false;Detail::fault=false;Detail::peerStopped=false;Detail::localStopped=true;
  Detail::txPausedForRf=false;Detail::flowDirty=true;Detail::haveLastPump=false;
  Detail::lastFlow=Detail::lastPump=0;stats=Stats{};Detail::unlock(state);pump();
#ifndef MF_UART_HOST_TEST
  timer1_isr_init();timer1_attachInterrupt(pump);timer1_enable(TIM_DIV16,TIM_EDGE,TIM_LOOP);timer1_write(500);
#endif
}
} // namespace UartLink
