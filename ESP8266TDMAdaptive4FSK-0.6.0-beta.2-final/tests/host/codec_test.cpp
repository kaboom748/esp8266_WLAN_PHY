#include "FskCodec.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace ESP8266FSKCodec;
int main(){
 for(unsigned n=0;n<16;n++){
  uint8_t count=0,code=encode(n);
  assert(decode(code,count)==n&&count==0);
  for(unsigned b=0;b<7;b++)assert(decode(code^(1u<<b),count)==n&&count==1);
 }
 const uint8_t check[]="123456789";
 assert(crc16(check,9)==0x29b1);
 for(unsigned value=0;value<256;value++){
  uint8_t data[4]={(uint8_t)value,(uint8_t)~value,(uint8_t)(value*37),(uint8_t)(value^0xa5)};
  uint8_t encoded[7];encodeBlock(data,4,encoded);
  for(unsigned start=0;start<56;start++)for(unsigned len=0;len<=8&&start+len<=56;len++){
   uint8_t damaged[7],out[4],count=0;memcpy(damaged,encoded,7);
   for(unsigned bit=start;bit<start+len;bit++)damaged[bit/8]^=1u<<(7-(bit%8));
   decodeBlock(damaged,out,count);assert(memcmp(data,out,4)==0);assert(count==len);
  }
 }
 puts("PASS: CRC16 standard vector; Hamming single-bit errors; every <=8-bit burst in 256 interleaved blocks");
}
