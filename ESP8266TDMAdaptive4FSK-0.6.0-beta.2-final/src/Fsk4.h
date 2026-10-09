#ifndef ESP8266_FSK4_H
#define ESP8266_FSK4_H
#include <stdint.h>
#include <string.h>

namespace Fsk4 {
static const uint16_t TONES[4]={1012,1020,4,12};
static const uint16_t TRAINING_SIZE=128;
inline uint8_t gray(uint8_t index){return index^(index>>1);}
inline uint8_t toneIndex(uint8_t dibit){return gray(dibit&3);}
inline uint8_t dibitAt(const uint8_t* packed,uint16_t bit){
  return (packed[bit>>3]>>(6-(bit&7)))&3;
}

// Learn four separate clusters, not four evenly spaced ADC values. Phase
// approximation and oscillator offsets can make the measured spacing unequal.
inline bool calibrate(const int32_t* input,uint16_t count,int32_t minSep,int32_t centers[4]){
  if(count<32 || count>TRAINING_SIZE || minSep<=0)return false;
  int32_t sorted[TRAINING_SIZE];
  memcpy(sorted,input,count*sizeof(int32_t));
  for(uint16_t i=1;i<count;i++){
    int32_t x=sorted[i];uint16_t j=i;
    while(j && sorted[j-1]>x){sorted[j]=sorted[j-1];j--;}
    sorted[j]=x;
  }
  uint16_t cuts[3]={};int64_t gaps[3]={};
  for(uint16_t i=1;i<count;i++){
    int64_t gap=int64_t(sorted[i])-sorted[i-1];
    for(uint8_t k=0;k<3;k++)if(gap>gaps[k]){
      for(uint8_t n=2;n>k;n--){gaps[n]=gaps[n-1];cuts[n]=cuts[n-1];}
      gaps[k]=gap;cuts[k]=i;break;
    }
  }
  if(gaps[2]<minSep)return false;
  for(uint8_t i=1;i<3;i++){
    uint16_t x=cuts[i];uint8_t j=i;
    while(j && cuts[j-1]>x){cuts[j]=cuts[j-1];j--;}cuts[j]=x;
  }
  int32_t candidate[4],spread[4];uint16_t first=0;
  for(uint8_t k=0;k<4;k++){
    uint16_t last=k<3?cuts[k]:count,n=last-first;
    if(n<4)return false;
    uint16_t a=first+n/4,b=first+(3*n)/4;
    candidate[k]=int32_t((int64_t(sorted[a])+sorted[b])/2);
    int64_t width=int64_t(sorted[b])-sorted[a];
    if(width>INT32_MAX)return false;
    spread[k]=int32_t(width);first=last;
  }
  for(uint8_t k=0;k<4;k++){
    int64_t sep=k?int64_t(candidate[k])-candidate[k-1]:INT32_MAX;
    if(k<3 && int64_t(candidate[k+1])-candidate[k]<sep)sep=int64_t(candidate[k+1])-candidate[k];
    if(sep<minSep || spread[k]>sep/3)return false;
  }
  memcpy(centers,candidate,sizeof(candidate));return true;
}

inline int8_t classify(int32_t hz,const int32_t centers[4]){
  uint8_t best=0;int64_t distance=INT64_MAX;
  for(uint8_t k=0;k<4;k++){
    int64_t delta=int64_t(hz)-centers[k];if(delta<0)delta=-delta;
    if(delta<distance){best=k;distance=delta;}
  }
  int64_t sep=best?int64_t(centers[best])-centers[best-1]:INT32_MAX;
  if(best<3 && int64_t(centers[best+1])-centers[best]<sep)sep=int64_t(centers[best+1])-centers[best];
  // Reject midpoint ambiguity and out-of-band estimates.
  return sep>0 && distance*20<sep*9?int8_t(best):int8_t(-1);
}

inline int8_t winner(const uint16_t votes[4]){
  uint8_t best=0;bool tie=false;
  for(uint8_t k=1;k<4;k++){
    if(votes[k]>votes[best]){best=k;tie=false;}
    else if(votes[k]==votes[best])tie=true;
  }
  return votes[best] && !tie?int8_t(best):int8_t(-1);
}
inline int8_t decide(const uint8_t bins[8][4],uint32_t bin){
  uint16_t votes[4]={};
  for(uint8_t k=0;k<5;k++)for(uint8_t tone=0;tone<4;tone++)votes[tone]+=bins[(bin-k)&7][tone];
  return winner(votes);
}
}
#endif
