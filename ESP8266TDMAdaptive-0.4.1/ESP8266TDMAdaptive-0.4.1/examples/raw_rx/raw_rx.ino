#include <RH_ESP8266FSK.h>
RH_ESP8266FSK driver(6);
void setup(){
  Serial.begin(115200);
  driver.setModemConfig(RH_ESP8266FSK::FSK_Rb333Reliable);
  driver.setThisAddress(2);
  if(!driver.init())Serial.println("INIT_FAIL");
}
void loop(){
  uint8_t data[RH_ESP8266FSK_MAX_MESSAGE_LEN],len=sizeof(data);
  if(driver.recv(data,&len)){
    Serial.printf("RX len=%u",len);
    for(uint8_t i=0;i<len;i++)Serial.printf(" %02X",data[i]);
    Serial.println();
  }
}
