#include <RH_ESP8266FSK.h>
RH_ESP8266FSK driver(6);
uint32_t nextSend=0,userIterations=0;
uint8_t sequence=0;
bool pending=false;
void setup(){
  Serial.begin(115200);
  driver.setModemConfig(RH_ESP8266FSK::FSK_Rb333Reliable);
  driver.setThisAddress(1);driver.setHeaderFrom(1);driver.setHeaderTo(2);
  if(!driver.init())Serial.println("INIT_FAIL");
  nextSend=millis()+2000;
}
void loop(){
  driver.poll();
  userIterations++;
  if(pending&&driver.mode()!=RHGenericDriver::RHModeTx){
    Serial.printf("sent=%u iterations=%lu\n",driver.waitPacketSent(),(unsigned long)userIterations);
    pending=false;nextSend=millis()+1500;
  }
  if(!pending&&(int32_t)(millis()-nextSend)>=0){
    uint8_t data[8]={sequence++,1,2,3,4,5,6,7};
    userIterations=0;pending=driver.send(data,sizeof(data));
    if(!pending)nextSend=millis()+1500;
  }
}
