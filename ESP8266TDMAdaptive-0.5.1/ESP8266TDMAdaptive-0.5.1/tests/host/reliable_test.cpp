#include <RHReliableDatagram.h>
#include <cassert>
#include <deque>
#include <vector>
#include <cstdio>
uint32_t fakeMs=0,tickMs=1;
long randomResult=0;
NullSerial Serial;
struct Frame{uint8_t from=2,to=1,id=1,flags=RH_FLAGS_ACK;std::vector<uint8_t> data={'!'};};
class FakeDriver:public RHGenericDriver{
public:
 std::deque<Frame> incoming;
 std::vector<Frame> sent;
 int response=0,skip=0;
 bool available()override{return !incoming.empty();}
 bool recv(uint8_t* b,uint8_t* n)override{
  if(incoming.empty())return false;
  Frame f=incoming.front();
  if(*n<f.data.size())return false;
  incoming.pop_front();
  _rxHeaderFrom=f.from;_rxHeaderTo=f.to;_rxHeaderId=f.id;_rxHeaderFlags=f.flags;
  *n=f.data.size();memcpy(b,f.data.data(),*n);return true;
 }
 bool send(const uint8_t* b,uint8_t n)override{
  Frame f;f.from=_txHeaderFrom;f.to=_txHeaderTo;f.id=_txHeaderId;f.flags=_txHeaderFlags;f.data.assign(b,b+n);sent.push_back(f);
  if(!(f.flags&RH_FLAGS_ACK)&&f.to!=255&&response&&sent.size()>(unsigned)skip){
   Frame a;a.from=f.to;a.to=f.from;a.id=f.id;
   if(response==2)a.from++;
   if(response==3)a.to++;
   if(response==4)a.id++;
   if(response==5)a.flags=0;
   if(response==6)a.data={'?'};
   if(response==7)a.data={'!',0};
   incoming.push_back(a);
  }
  return true;
 }
 uint8_t maxMessageLength()override{return 32;}
};
int main(){
 uint8_t payload=42;
 for(int response=1;response<=7;response++){
  FakeDriver d;d.response=response;
  RHReliableDatagram m(d,1);assert(m.init());m.setTimeout(3);m.setRetries(2);
  assert(m.sendtoWait(&payload,1,2)==(response==1));
  assert(d.sent.size()==(response==1?1u:3u));
  if(response!=1){assert(m.retransmissions()==2);assert(d.sent[0].id==d.sent[2].id);assert(d.sent[1].flags&RH_FLAGS_RETRY);}
 }
 {
  FakeDriver d;d.response=1;d.skip=1;RHReliableDatagram m(d,1);m.init();m.setTimeout(3);
  assert(m.sendtoWait(&payload,1,2));assert(m.retransmissions()==1);
 }
 {
  FakeDriver d;RHReliableDatagram m(d,1);m.init();m.setTimeout(1);m.setRetries(255);
  assert(!m.sendtoWait(&payload,1,2));assert(d.sent.size()==256);
 }
 {
  FakeDriver d;RHReliableDatagram m(d,1);m.init();m.setTimeout(40000);m.setRetries(0);
  fakeMs=0xfffffff0;tickMs=1000;randomResult=39999;uint32_t start=fakeMs;
  assert(!m.sendtoWait(&payload,1,2));assert((uint32_t)(fakeMs-start)>=79999);
  tickMs=1;randomResult=0;
 }
 {
  FakeDriver d;RHReliableDatagram m(d,1);m.init();m.setAckTiming(0);
  Frame f;f.id=0;f.flags=RH_FLAGS_RETRY;f.data={23};d.incoming.push_back(f);
  uint8_t b[32],n=32;
  assert(m.recvfromAck(b,&n));assert(b[0]==23&&n==1);assert(d.sent.size()==1);
  d.incoming.push_back(f);n=32;assert(!m.recvfromAck(b,&n));assert(d.sent.size()==2);
  f.flags=0;d.incoming.push_back(f);n=32;assert(m.recvfromAck(b,&n));
  f.to=255;f.id++;d.incoming.push_back(f);n=32;size_t ackCount=d.sent.size();
  assert(m.recvfromAck(b,&n));assert(d.sent.size()==ackCount);
 }
 puts("PASS: ACK identity/payload, retries, 255 retry bound, time wrap, deduplication, broadcast");
}
