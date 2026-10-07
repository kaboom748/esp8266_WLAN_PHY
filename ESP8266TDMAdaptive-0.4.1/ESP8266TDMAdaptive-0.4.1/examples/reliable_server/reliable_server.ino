#include <RH_ESP8266FSK.h>
#include <RHReliableDatagram.h>
RH_ESP8266FSK driver(6);
RHReliableDatagram manager(driver,2);
void setup(){
  Serial.begin(115200);
  driver.setModemConfig(RH_ESP8266FSK::FSK_Rb333Reliable);
  if(!manager.init())Serial.println("INIT_FAIL");
}
void loop(){
  uint8_t data[RH_ESP8266FSK_MAX_MESSAGE_LEN],len=sizeof(data),from=0,id=0;
  if(manager.recvfromAck(data,&len,&from,NULL,&id)){
    Serial.printf("from=%u id=%u len=%u",from,id,len);
    for(uint8_t i=0;i<len;i++)Serial.printf(" %02X",data[i]);
    Serial.println();
  }
}
