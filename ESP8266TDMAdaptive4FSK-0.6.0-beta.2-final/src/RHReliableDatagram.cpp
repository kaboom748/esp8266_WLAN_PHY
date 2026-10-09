#include "RHReliableDatagram.h"

RHReliableDatagram::RHReliableDatagram(RHGenericDriver& driver,uint8_t address)
  :RHDatagram(driver,address),_retransmissions(0),_lastSequenceNumber(0),
   _timeout(RH_DEFAULT_TIMEOUT),_retries(RH_DEFAULT_RETRIES),
   _ackDelayMs(100),_ackRepeats(1),_ackGapMs(0),_useAckBeacon(false){
  memset(_seenIds,0,sizeof(_seenIds));
  memset(_seenValid,0,sizeof(_seenValid));
}
void RHReliableDatagram::setTimeout(uint16_t ms){_timeout=ms?ms:1;}
void RHReliableDatagram::setRetries(uint8_t count){_retries=count;}
uint8_t RHReliableDatagram::retries(){return _retries;}
void RHReliableDatagram::setAckTiming(uint16_t ms,uint8_t repeats,uint16_t gap){
  _ackDelayMs=ms;_ackRepeats=repeats?repeats:1;_ackGapMs=gap;
}
// Kept for source compatibility. Unaddressed tone beacons cannot confirm delivery.
void RHReliableDatagram::setUseAckBeacon(bool){_useAckBeacon=false;}
uint32_t RHReliableDatagram::retransmissions(){return _retransmissions;}
void RHReliableDatagram::resetRetransmissions(){_retransmissions=0;}

bool RHReliableDatagram::sendtoWait(uint8_t* data,uint8_t len,uint8_t address){
  uint8_t id=++_lastSequenceNumber;
  for(uint16_t attempt=0;attempt<=(uint16_t)_retries;attempt++){
    _driver.setHeaderId(id);
    _driver.setHeaderFlags(attempt?RH_FLAGS_RETRY:0,RH_FLAGS_ACK|RH_FLAGS_RETRY);
    if(!sendto(data,len,address)||!waitPacketSent())return false;
    if(address==RH_BROADCAST_ADDRESS)return true;
    uint32_t waitMs=(uint32_t)_timeout+(uint32_t)random(_timeout);
    uint32_t started=millis();
    while((uint32_t)(millis()-started)<waitMs){
      uint8_t ack[RH_ESP8266FSK_ACK_BUFFER_LEN];
      uint8_t n=sizeof(ack),from=0,to=0,ackId=0,flags=0;
      if(recvfrom(ack,&n,&from,&to,&ackId,&flags)){
        if(from==address&&to==_thisAddress&&ackId==id&&
           (flags&RH_FLAGS_ACK)&&n==1&&ack[0]=='!')return true;
      }
      yield();
    }
    if(attempt<_retries)_retransmissions++;
  }
  return false;
}

void RHReliableDatagram::acknowledge(uint8_t id,uint8_t from){
  delay(_ackDelayMs);
  uint8_t payload='!';
  _driver.setHeaderId(id);
  _driver.setHeaderFlags(RH_FLAGS_ACK,RH_FLAGS_ACK|RH_FLAGS_RETRY);
  for(uint8_t i=0;i<_ackRepeats;i++){
    if(!sendto(&payload,1,from))break;
    waitPacketSent();
    if(i+1<_ackRepeats&&_ackGapMs)delay(_ackGapMs);
  }
  _driver.setHeaderFlags(0,RH_FLAGS_ACK|RH_FLAGS_RETRY);
}
bool RHReliableDatagram::haveNewMessage(uint8_t from,uint8_t id,uint8_t flags){
  uint8_t mask=1u<<(from&7);
  bool seen=(_seenValid[from>>3]&mask)!=0;
  if((flags&RH_FLAGS_RETRY)&&seen&&_seenIds[from]==id)return false;
  _seenIds[from]=id;_seenValid[from>>3]|=mask;
  return true;
}
bool RHReliableDatagram::recvfromAck(uint8_t* data,uint8_t* len,uint8_t* from,
                                  uint8_t* to,uint8_t* id,uint8_t* flags){
  uint8_t f=0,t=0,i=0,fl=0;
  if(!recvfrom(data,len,&f,&t,&i,&fl)||(fl&RH_FLAGS_ACK))return false;
  if(t!=RH_BROADCAST_ADDRESS)acknowledge(i,f);
  bool fresh=haveNewMessage(f,i,fl);
  if(from)*from=f;
  if(to)*to=t;
  if(id)*id=i;
  if(flags)*flags=fl;
  return fresh;
}
bool RHReliableDatagram::recvfromAckTimeout(uint8_t* data,uint8_t* len,uint16_t timeout,
                                         uint8_t* from,uint8_t* to,uint8_t* id,uint8_t* flags){
  if(!len)return false;
  uint8_t capacity=*len;
  uint32_t started=millis();
  do{
    uint8_t n=capacity;
    if(recvfromAck(data,&n,from,to,id,flags)){*len=n;return true;}
    yield();
  }while((uint32_t)(millis()-started)<timeout);
  return false;
}
