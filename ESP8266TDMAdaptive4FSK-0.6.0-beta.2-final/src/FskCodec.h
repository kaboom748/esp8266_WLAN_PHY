#pragma once
#include <stdint.h>
namespace ESP8266FSKCodec {
inline uint8_t encode(uint8_t n){
  uint8_t a=(n>>3)&1,b=(n>>2)&1,c=(n>>1)&1,d=n&1;
  return ((a^b^d)<<6)|((a^c^d)<<5)|(a<<4)|((b^c^d)<<3)|(b<<2)|(c<<1)|d;
}
inline uint8_t decode(uint8_t code,uint8_t& corrected){
  uint8_t a=(code>>6)&1,b=(code>>5)&1,c=(code>>4)&1,d=(code>>3)&1;
  uint8_t e=(code>>2)&1,f=(code>>1)&1,g=code&1;
  uint8_t syndrome=(a^c^e^g)|((b^c^f^g)<<1)|((d^e^f^g)<<2);
  corrected=syndrome?1:0;
  if(syndrome)code^=1u<<(7-syndrome);
  return (((code>>4)&1)<<3)|(((code>>2)&1)<<2)|(((code>>1)&1)<<1)|(code&1);
}
inline uint16_t crc16(const uint8_t* data,uint8_t len){
  uint16_t crc=0xffff;
  for(uint8_t i=0;i<len;i++){
    crc^=(uint16_t)data[i]<<8;
    for(uint8_t bit=0;bit<8;bit++)crc=(crc&0x8000)?(crc<<1)^0x1021:crc<<1;
  }
  return crc;
}
// Eight Hamming words are transmitted column-wise to spread short error bursts.
inline void encodeBlock(const uint8_t* data,uint8_t len,uint8_t block[7]){
  uint8_t words[8]={};
  for(uint8_t i=0;i<4;i++){
    uint8_t value=i<len?data[i]:0;
    words[2*i]=encode(value>>4);words[2*i+1]=encode(value&15);
  }
  for(uint8_t row=0;row<7;row++){
    block[row]=0;
    for(uint8_t col=0;col<8;col++)block[row]|=((words[col]>>(6-row))&1)<<(7-col);
  }
}
inline void decodeBlock(const uint8_t block[7],uint8_t data[4],uint8_t& corrections){
  corrections=0;
  for(uint8_t col=0;col<8;col++){
    uint8_t word=0,corrected=0;
    for(uint8_t row=0;row<7;row++)word=(word<<1)|((block[row]>>(7-col))&1);
    uint8_t n=decode(word,corrected);corrections+=corrected;
    if(!(col&1))data[col/2]=n<<4;else data[col/2]|=n;
  }
}
}
