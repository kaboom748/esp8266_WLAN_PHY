#include <RH_ESP8266FSK.h>
#include <RHReliableDatagram.h>
RH_ESP8266FSK driver(6);
RHReliableDatagram manager(driver,1);
uint8_t sequence=0;
uint32_t nextSend=0;
void setup(){
  Serial.begin(115200);
  driver.setModemConfig(RH_ESP8266FSK::FSK_Rb333Reliable);
  if(!manager.init()){Serial.println("INIT_FAIL");return;}
  manager.setTimeout(1100);
  manager.setRetries(5);
  nextSend=millis()+2000;
}
void loop(){
  if((int32_t)(millis()-nextSend)<0)return;
  uint8_t data[8]={sequence++,0x23,0xa5,0x00,0xff,0x5a,0x81,0x42};
  uint32_t started=millis();
  bool ok=manager.sendtoWait(data,sizeof(data),2);
  Serial.printf("seq=%u ack=%u ms=%lu heap=%u\n",data[0],ok,
                (unsigned long)(millis()-started),ESP.getFreeHeap());
  nextSend=millis()+1500;
}
