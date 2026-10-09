#ifndef ESP8266_FSK_FRAME_H
#define ESP8266_FSK_FRAME_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "FskCodec.h"

namespace FskFrame {
static const uint8_t MAX_PAYLOAD=32,RAW_SIZE=39;
inline uint16_t bitCount(uint8_t length,uint8_t preamble){
  return preamble+12+14+56*((length+9)/4);
}
inline void append(uint8_t* wire,uint16_t& count,bool bit){
  if(bit)wire[count>>3]|=1u<<(7-(count&7));
  count++;
}
inline uint16_t encode(const uint8_t* data,uint8_t length,const uint8_t headers[4],
                       uint8_t preamble,uint8_t* wire,size_t capacity){
  if(length>MAX_PAYLOAD || (!data && length) || preamble<24 || preamble>64 ||
     (preamble&1) || capacity<(bitCount(length,preamble)+7u)/8u)return 0;
  uint8_t raw[RAW_SIZE];raw[0]=length+4;
  memcpy(raw+1,headers,4);if(length)memcpy(raw+5,data,length);
  uint8_t n=length+5;uint16_t crc=ESP8266FSKCodec::crc16(raw,n);
  raw[n++]=crc>>8;raw[n++]=crc;
  memset(wire,0,capacity);uint16_t count=0;
  for(uint8_t i=0;i<preamble;i++)append(wire,count,i&1);
  for(int8_t i=11;i>=0;i--)append(wire,count,(0xb38>>i)&1);
  for(int8_t half=1;half>=0;half--){
    uint8_t h=ESP8266FSKCodec::encode((raw[0]>>(half*4))&15);
    for(int8_t k=6;k>=0;k--)append(wire,count,(h>>k)&1);
  }
  for(uint8_t i=1;i<n;i+=4){
    uint8_t block[7],size=n-i;if(size>4)size=4;
    ESP8266FSKCodec::encodeBlock(raw+i,size,block);
    for(uint8_t row=0;row<7;row++)for(int8_t bit=7;bit>=0;bit--)append(wire,count,(block[row]>>bit)&1);
  }
  return count;
}

struct Decoder {
  enum Result { NONE,SYNC,CORRECTION_REJECT,CRC_REJECT,FRAME };
  uint32_t shift=0;
  uint8_t code=0,codeBits=0,high=0,nibble=0,count=0,expected=0,corrections=0;
  bool active=false,inverted=false;
  uint8_t block[7]={},raw[RAW_SIZE]={};

  Result feed(bool bit,bool firstInSymbol,bool fourFsk,uint8_t maxCorrections){
    if(!active){
      shift=((shift<<1)|bit)&0xffffff;
      if(fourFsk && firstInSymbol)return NONE;
      uint32_t normal=shift^0x555b38u;
      // Reversing the order of Gray-coded 4-FSK tones flips only the MSB.
      uint32_t inverse=shift^(0x555b38u^(fourFsk?0xaaaaaau:0xffffffu));
      bool a=__builtin_popcount(normal&0xfff)<=1&&__builtin_popcount(normal>>12)<=2;
      bool b=__builtin_popcount(inverse&0xfff)<=1&&__builtin_popcount(inverse>>12)<=2;
      if(!a && !b)return NONE;
      inverted=b;active=true;code=0;codeBits=0;nibble=0;count=0;expected=0;corrections=0;
      return SYNC;
    }
    bit^=inverted && (!fourFsk || firstInSymbol);
    if(count==0){
      code=(code<<1)|bit;if(++codeBits<7)return NONE;
      uint8_t corrected=0,value=ESP8266FSKCodec::decode(code,corrected);
      corrections+=corrected;code=0;codeBits=0;
      if(!nibble){high=value;nibble=1;return NONE;}
      raw[0]=(high<<4)|value;count=1;nibble=0;
      if(raw[0]<4 || raw[0]>4+MAX_PAYLOAD){active=false;shift=0;return NONE;}
      expected=raw[0]+3;memset(block,0,sizeof(block));return NONE;
    }
    if(bit)block[codeBits>>3]|=1u<<(7-(codeBits&7));
    if(++codeBits<56)return NONE;
    uint8_t bytes[4],corrected=0;
    ESP8266FSKCodec::decodeBlock(block,bytes,corrected);
    corrections+=corrected;codeBits=0;memset(block,0,sizeof(block));
    if(corrections>maxCorrections){active=false;shift=0;return CORRECTION_REJECT;}
    for(uint8_t i=0;i<4&&count<expected;i++)raw[count++]=bytes[i];
    if(count<expected)return NONE;
    active=false;shift=0;
    return ESP8266FSKCodec::crc16(raw,count-2)==((uint16_t)raw[count-2]<<8|raw[count-1])?FRAME:CRC_REJECT;
  }
};
}
#endif
