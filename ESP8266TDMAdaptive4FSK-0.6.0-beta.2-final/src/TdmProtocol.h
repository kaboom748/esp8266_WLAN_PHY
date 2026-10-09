#ifndef ESP8266_TDM_PROTOCOL_H
#define ESP8266_TDM_PROTOCOL_H
#include <stdint.h>
#include <string.h>
#include "TdmRate.h"

namespace TdmProtocol {
static const uint8_t WIRE_SIZE=32, PAYLOAD_SIZE=14, MAGIC=0xd6;
enum { DATA=1, ACK=2, FIRST=4, LAST=8 };
struct Cell {
  uint8_t flags=0;
  uint8_t rate=0, requestedRate=0;
  uint16_t cycle=0;
  uint32_t session=0, seenSession=0;
  uint16_t dataId=0, ackId=0;
  uint8_t length=0, corrections=0;
  uint8_t payload[PAYLOAD_SIZE]={};
};
inline bool due(uint32_t now,uint32_t deadline){return int32_t(now-deadline)>=0;}
inline bool newer(uint16_t a,uint16_t b){return int16_t(a-b)>0;}
inline void put16(uint8_t* p,uint16_t x){p[0]=x;p[1]=x>>8;}
inline void put32(uint8_t* p,uint32_t x){put16(p,x);put16(p+2,x>>16);}
inline uint16_t get16(const uint8_t* p){return uint16_t(p[0])|uint16_t(p[1])<<8;}
inline uint32_t get32(const uint8_t* p){return uint32_t(get16(p))|uint32_t(get16(p+2))<<16;}
inline void encode(const Cell& c,uint8_t* p){
  memset(p,0x55,WIRE_SIZE);
  p[0]=MAGIC;p[1]=(c.flags&3)|(c.requestedRate<<2)|(c.rate<<5);put16(p+2,c.cycle);
  put32(p+4,c.session);put32(p+8,c.seenSession);
  put16(p+12,c.dataId);put16(p+14,c.ackId);
  p[16]=c.length;p[17]=c.corrections|((c.flags&12)<<3);
  if(c.length<=PAYLOAD_SIZE)memcpy(p+18,c.payload,c.length);
}
inline bool decode(const uint8_t* p,uint8_t n,Cell& c){
  if(n!=WIRE_SIZE || p[0]!=MAGIC || p[16]>PAYLOAD_SIZE || (p[17]&31)>16 || (p[17]&128))return false;
  if((p[1]>>5)>=TdmRate::COUNT || ((p[1]>>2)&7)>=TdmRate::COUNT)return false;
  if(bool(p[1]&DATA)!=(p[16]!=0) || get32(p+4)==0)return false;
  if((p[1]&ACK) && get32(p+8)==0)return false;
  c.flags=(p[1]&3)|((p[17]>>3)&12);c.rate=p[1]>>5;c.requestedRate=(p[1]>>2)&7;
  c.cycle=get16(p+2);c.session=get32(p+4);c.seenSession=get32(p+8);
  c.dataId=get16(p+12);c.ackId=get16(p+14);c.length=p[16];c.corrections=p[17]&31;
  memcpy(c.payload,p+18,PAYLOAD_SIZE);
  return true;
}
}
#endif
