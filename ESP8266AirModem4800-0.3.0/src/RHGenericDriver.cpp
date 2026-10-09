#include "RHGenericDriver.h"
RHGenericDriver::RHGenericDriver():_mode(RHModeInitialising),_thisAddress(RH_BROADCAST_ADDRESS),_txHeaderTo(RH_BROADCAST_ADDRESS),_txHeaderFrom(RH_BROADCAST_ADDRESS),_txHeaderId(0),_txHeaderFlags(0),_rxHeaderTo(0),_rxHeaderFrom(0),_rxHeaderId(0),_rxHeaderFlags(0),_promiscuous(false),_lastRssi(0),_rxBad(0),_rxGood(0),_txGood(0),_cad_timeout(0) {}
bool RHGenericDriver::init(){_mode=RHModeIdle;return true;}
void RHGenericDriver::waitAvailable(){while(!available()) yield();}
bool RHGenericDriver::waitPacketSent(){while(_mode==RHModeTx) yield(); return true;}
bool RHGenericDriver::waitPacketSent(uint16_t timeout){uint32_t t=millis(); while(_mode==RHModeTx){if((uint32_t)(millis()-t)>=timeout)return false; yield();} return true;}
bool RHGenericDriver::waitAvailableTimeout(uint16_t timeout){uint32_t t=millis(); while((uint32_t)(millis()-t)<timeout){if(available())return true; yield();} return false;}
bool RHGenericDriver::waitCAD(){return true;} bool RHGenericDriver::isChannelActive(){return false;} void RHGenericDriver::setCADTimeout(unsigned long t){_cad_timeout=t;}
void RHGenericDriver::setThisAddress(uint8_t a){_thisAddress=a;} void RHGenericDriver::setHeaderTo(uint8_t v){_txHeaderTo=v;} void RHGenericDriver::setHeaderFrom(uint8_t v){_txHeaderFrom=v;} void RHGenericDriver::setHeaderId(uint8_t v){_txHeaderId=v;} void RHGenericDriver::setHeaderFlags(uint8_t set,uint8_t clear){_txHeaderFlags&=~clear;_txHeaderFlags|=set;}
void RHGenericDriver::setPromiscuous(bool p){_promiscuous=p;} uint8_t RHGenericDriver::headerTo(){return _rxHeaderTo;} uint8_t RHGenericDriver::headerFrom(){return _rxHeaderFrom;} uint8_t RHGenericDriver::headerId(){return _rxHeaderId;} uint8_t RHGenericDriver::headerFlags(){return _rxHeaderFlags;} int16_t RHGenericDriver::lastRssi(){return _lastRssi;} RHGenericDriver::RHMode RHGenericDriver::mode(){return _mode;} void RHGenericDriver::setMode(RHMode m){_mode=m;} bool RHGenericDriver::sleep(){_mode=RHModeSleep;return true;} uint16_t RHGenericDriver::rxBad(){return _rxBad;} uint16_t RHGenericDriver::rxGood(){return _rxGood;} uint16_t RHGenericDriver::txGood(){return _txGood;}
void RHGenericDriver::printBuffer(const char* prompt,const uint8_t* buf,uint8_t len){Serial.print(prompt);for(uint8_t i=0;i<len;i++){Serial.print(' '); if(buf[i]<16)Serial.print('0'); Serial.print(buf[i],HEX);} Serial.println();}

bool RHGenericDriver::supportsAckBeacon(){return false;}
bool RHGenericDriver::sendAckBeacon(uint8_t,uint16_t){return false;}
bool RHGenericDriver::waitAckBeacon(uint16_t){return false;}
