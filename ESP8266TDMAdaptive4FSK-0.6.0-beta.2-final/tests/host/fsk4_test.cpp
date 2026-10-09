#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "Fsk4.h"
#include "FskFrame.h"

using FskFrame::Decoder;
static unsigned delivered=0;
static uint32_t rng=0x19d53;
static uint32_t nextRandom(){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}

static bool decode(const uint8_t* packed,uint16_t bits,bool reversed,
                   const uint8_t* data,uint8_t length,int flipBit=-1,bool four=true,uint8_t maxCorrections=16){
  Decoder d{};
  bool found=false;
  for(uint16_t i=0;i<bits;i++){
    bool bit=(packed[i>>3]>>(7-(i&7)))&1;
    if(i==flipBit)bit=!bit;
    if(reversed && (!four || !(i&1)))bit=!bit;
    auto result=d.feed(bit,!(i&1),four,maxCorrections);
    if(result==Decoder::FRAME){
      assert(d.count==length+7);
      assert(d.raw[0]==length+4);
      assert(!memcmp(d.raw+5,data,length));
      assert(d.inverted==reversed);
      found=true;delivered++;
    }
  }
  return found;
}

int main(){
  for(uint8_t i=0;i<4;i++){
    assert(Fsk4::toneIndex(Fsk4::gray(i))==i);
    assert(Fsk4::gray(3-i)==(Fsk4::gray(i)^2));
    if(i)assert(__builtin_popcount(Fsk4::gray(i)^Fsk4::gray(i-1))==1);
  }
  uint8_t headers[4]={2,1,0xa9,0x73},data[32],wire[82];
  for(unsigned trial=0;trial<20;trial++)for(uint8_t length=0;length<=32;length++){
    for(uint8_t& byte:data)byte=nextRandom();
    uint16_t bits=FskFrame::encode(data,length,headers,64,wire,sizeof(wire));
    assert(bits==FskFrame::bitCount(length,64) && !(bits&1));
    for(uint16_t bit=0;bit<bits;bit+=2){
      uint8_t index=Fsk4::toneIndex(Fsk4::dibitAt(wire,bit));
      assert(Fsk4::gray(index)==Fsk4::dibitAt(wire,bit));
    }
    for(bool four:{false,true})for(bool reverse:{false,true}){
      assert(decode(wire,bits,reverse,data,length,-1,four));
      assert(!decode(wire,bits-2,reverse,data,length,-1,four));
    }
    if(trial==0)for(uint16_t bit=76;bit<bits;bit++){
      assert(decode(wire,bits,false,data,length,bit));
      assert(decode(wire,bits,true,data,length,bit));
    }
  }
  assert(FskFrame::bitCount(32,64)==650);
  assert(FskFrame::encode(data,33,headers,64,wire,sizeof(wire))==0);
  assert(FskFrame::encode(nullptr,1,headers,64,wire,sizeof(wire))==0);
  assert(FskFrame::encode(data,32,headers,63,wire,sizeof(wire))==0);
  assert(FskFrame::encode(data,32,headers,64,wire,sizeof(wire)-1)==0);
  // Re-encode an altered header with valid Hamming words but the old CRC.
  uint16_t corruptBits=FskFrame::encode(data,32,headers,64,wire,sizeof(wire));
  uint8_t altered[4]={3,1,0xa9,0x73},block[7];
  ESP8266FSKCodec::encodeBlock(altered,4,block);
  for(uint16_t k=0;k<56;k++){
    uint16_t bit=90+k;uint8_t mask=1u<<(7-(bit&7));
    if((block[k>>3]>>(7-(k&7)))&1)wire[bit>>3]|=mask;else wire[bit>>3]&=~mask;
  }
  assert(!decode(wire,corruptBits,false,data,32));
  FskFrame::encode(data,32,headers,64,wire,sizeof(wire));
  assert(!decode(wire,650,false,data,32,100,true,0));

  int32_t samples[128],centers[4];
  const int32_t nominal[4]={-97000,-61000,-12000,31000};
  for(uint16_t i=0;i<128;i++)samples[i]=nominal[i/32]+int32_t(i%7)*190-570;
  assert(Fsk4::calibrate(samples,128,12000,centers));
  for(uint8_t k=0;k<4;k++){
    assert(Fsk4::classify(nominal[k],centers)==k);
    assert(Fsk4::classify(nominal[k]+3000,centers)==k);
    if(k<3)assert(Fsk4::classify((centers[k]+centers[k+1])/2,centers)==-1);
  }
  assert(Fsk4::classify(-200000,centers)==-1);
  assert(Fsk4::classify(150000,centers)==-1);
  uint16_t empty[4]={},tie[4]={3,3,1,0},unique[4]={2,3,7,1};
  assert(Fsk4::winner(empty)==-1 && Fsk4::winner(tie)==-1 && Fsk4::winner(unique)==2);
  for(uint16_t i=0;i<128;i++)samples[i]=nominal[(i/32)%3];
  assert(!Fsk4::calibrate(samples,128,12000,centers));
  for(uint16_t i=0;i<128;i++)samples[i]=int32_t(i/32)*5000;
  assert(!Fsk4::calibrate(samples,128,12000,centers));
  for(uint16_t i=0;i<128;i++)samples[i]=1000;
  assert(!Fsk4::calibrate(samples,128,12000,centers));
  // Sliding one-millisecond samples across four 40-ms calibration pilots.
  uint16_t count=0,pos=0;bool acquired=false;
  for(uint16_t ms=0;ms<160;ms++){
    samples[pos++]=nominal[ms/40]+int32_t(ms%5)*100;pos%=128;
    if(count<128)count++;
    if(!(pos%4) && Fsk4::calibrate(samples,count,12000,centers))acquired=true;
  }
  assert(acquired);
  // End-to-end synthetic frequency observations; not a hardware/ADC model.
  for(bool reverse:{false,true}){
    for(uint8_t& byte:data)byte=nextRandom();
    uint16_t bits=FskFrame::encode(data,32,headers,64,wire,sizeof(wire));
    Decoder d{};bool found=false;
    for(uint16_t i=0;i<bits;i+=2){
      uint8_t tone=Fsk4::toneIndex(Fsk4::dibitAt(wire,i));
      if(reverse)tone=3-tone;
      int8_t detected=Fsk4::classify(nominal[tone]+int32_t(nextRandom()%2001)-1000,centers);
      assert(detected>=0);uint8_t dibit=Fsk4::gray(detected);
      assert(d.feed((dibit>>1)&1,true,true,16)!=Decoder::FRAME);
      if(d.feed(dibit&1,false,true,16)==Decoder::FRAME){
        assert(!memcmp(d.raw+5,data,32));found=true;
      }
    }
    assert(found);
  }
  // Exercise the production five-bin vote and packet decoder at eight phases,
  // using synthetic estimates every 27 us with bounded noise and timing jitter.
  for(uint32_t period:{1000u,800u,600u,500u,400u,300u,200u,140u})for(bool reverse:{false,true}){
    for(uint32_t offset=0;offset<period;offset+=17){
      uint16_t bits=FskFrame::encode(data,32,headers,64,wire,sizeof(wire));
      uint8_t bins[8][4]={};Decoder phases[8]={};uint32_t bin=0;
      bool found=false;
      for(uint32_t t=0;t<offset+(bits/2+2)*period;t+=27+(nextRandom()%2)){
        uint32_t nextBin=t*8/period;
        while(bin<nextBin){
          Decoder& d=phases[bin&7];int8_t tone=Fsk4::decide(bins,bin);
          if(tone>=0){
            uint8_t dibit=Fsk4::gray(tone);
            assert(d.feed((dibit>>1)&1,true,true,16)!=Decoder::FRAME);
            if(d.feed(dibit&1,false,true,16)==Decoder::FRAME){
              assert(!memcmp(d.raw+5,data,32));found=true;
            }
          }else {d.active=false;d.shift=0;}
          bin++;memset(bins[bin&7],0,4);
        }
        int8_t tone=1;
        if(t>=offset){
          uint32_t symbol=(t-offset)/period;
          if(symbol<bits/2)tone=Fsk4::toneIndex(Fsk4::dibitAt(wire,symbol*2));
        }
        if(reverse)tone=3-tone;
        int8_t detected=Fsk4::classify(nominal[tone]+int32_t(nextRandom()%2001)-1000,centers);
        if(detected>=0)bins[bin&7][detected]++;
      }
      assert(found);
    }
  }
  printf("PASS 4-FSK: Gray mapping, both orientations, %u frame decodes, all lengths, single-bit FEC, truncation, four-cluster pilots, ambiguity rejection, synthetic frequency round trip\n",delivered);
  puts("PASS 4-FSK timing: production vote/decoder, eight symbol periods, phase offsets, synthetic noise/jitter; CRC and correction-limit rejection");
}
