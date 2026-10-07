#include <RH_ESP8266FSK.h>
#include <RHReliableDatagram.h>
extern "C" {
#include "user_interface.h"
}
RH_ESP8266FSK driver;
RHReliableDatagram manager(driver,2);
uint8_t sequence=0,payloadLength=1;
bool listening=false;
uint32_t statsStarted=0,userLoops=0;
void setup(){
  Serial.begin(115200);delay(200);
  driver.setTxRepeats(1);
  if(!manager.init()){Serial.println("INIT_FAIL");return;}
  manager.setTimeout(1100);manager.setRetries(5);
  manager.setAckTiming(100,1,0);
  statsStarted=micros();
  Serial.printf("READY heap=%u cpu=%u\n",ESP.getFreeHeap(),ESP.getCpuFreqMHz());
}
void loop(){
  userLoops++;
  if(Serial.available()){
    char c=Serial.read();
    if(c=='1'||c=='2'||c=='3'){manager.setThisAddress(c-'0');Serial.printf("ADDR %u\n",manager.thisAddress());}
    if(c=='l'){listening=true;Serial.println("LISTEN");}
    if(c=='q'){listening=false;Serial.println("QUIET");}
    if(c=='x')payloadLength=1;
    if(c=='y')payloadLength=8;
    if(c=='z')payloadLength=32;
    if(c=='n')driver.setBitUs(3000);
    if(c=='f')driver.setBitUs(2000);
    if(c=='v')driver.setBitUs(4000);
    if(c=='k')driver.setBitUs(1500);
    if(c=='8')system_update_cpu_freq(80);
    if(c=='6')system_update_cpu_freq(160);
    if(c=='c'){driver.resetStatistics();statsStarted=micros();userLoops=0;}
    if(c=='t'){
      auto st=driver.statistics();
      Serial.printf("STATS elapsedUs=%lu workUs=%lu maxUs=%lu samples=%lu polls=%lu slow=%lu loops=%lu heap=%u cpu=%u bitUs=%lu\n",
        (unsigned long)(micros()-statsStarted),(unsigned long)st.workUs,(unsigned long)st.maxPollUs,
        (unsigned long)st.samples,(unsigned long)st.polls,(unsigned long)st.slowPolls,
        (unsigned long)userLoops,ESP.getFreeHeap(),ESP.getCpuFreqMHz(),(unsigned long)driver.bitUs());
      Serial.printf("TX_STATS lateUs=%lu aborts=%lu gate=%lu\n",(unsigned long)st.txLateUs,(unsigned long)st.txAborts,(unsigned long)((*(volatile uint32_t*)0x600005b8u>>18)&1u));
    }
    if(c=='i'||c=='h'||c=='w'){
      bool outcome=false;
      if(c=='i')driver.setMode(RHGenericDriver::RHModeIdle);
      if(c=='h')outcome=driver.sleep();
      if(c=='w')outcome=driver.waitPacketSent(1);
      Serial.printf("STOP kind=%c result=%u sent=%u gate=%lu\n",c,outcome,
        driver.waitPacketSent(),(unsigned long)((*(volatile uint32_t*)0x600005b8u>>18)&1u));
    }
    if(c=='r'||c=='s'){
      uint8_t data[32];data[0]=sequence++;
      for(uint8_t i=1;i<payloadLength;i++)data[i]=(data[0]*37+i*19)^0xa5;
      uint32_t started=millis(),retries=manager.retransmissions();
      bool ok=c=='s'?manager.sendtoWait(data,payloadLength,manager.thisAddress()==1?2:1):
                     manager.sendto(data,payloadLength,manager.thisAddress()==1?2:1);
      Serial.printf("RESULT seq=%u len=%u ok=%u ms=%lu retry=%lu\n",data[0],payloadLength,ok,
        (unsigned long)(millis()-started),(unsigned long)(manager.retransmissions()-retries));
    }
  }
  if(listening){
    uint8_t data[32],n=sizeof(data),from=0,id=0;
    if(manager.recvfromAck(data,&n,&from,NULL,&id)){
      bool valid=n>0;
      for(uint8_t i=1;i<n;i++)if(data[i]!=(uint8_t)((data[0]*37+i*19)^0xa5))valid=false;
      auto cal=driver.lastCalibration();
      Serial.printf("RX seq=%u len=%u valid=%u id=%u from=%u zero=%ld one=%ld sep=%ld\n",
        data[0],n,valid,id,from,(long)cal.zeroHz,(long)cal.oneHz,(long)cal.sepHz);
    }
  }
  if(driver.mode()==RHGenericDriver::RHModeTx)driver.poll();
  yield();
}
