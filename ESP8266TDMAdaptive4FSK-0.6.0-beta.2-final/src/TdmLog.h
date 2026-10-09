#ifndef ESP8266_TDM_LOG_H
#define ESP8266_TDM_LOG_H
#include <stdarg.h>
#include <stdio.h>
#include <Arduino.h>

class TdmLog {
public:
  void printf(const char* format,...){
    char line[768];va_list args;va_start(args,format);
    int n=vsnprintf(line,sizeof(line),format,args);va_end(args);
    if(n<0 || size_t(n)>=sizeof(line) || size_t(n)>sizeof(_data)-_size){_dropped++;return;}
    for(int i=0;i<n;i++)_data[(_head+_size+i)%sizeof(_data)]=line[i];
    _size+=n;
  }
  void println(const char* text){printf("%s\n",text);}
  void service(uint32_t remaining){
    if(remaining<=5000 || !_size)return;
    size_t length=0;
    while(length<_size && _data[(_head+length)%sizeof(_data)]!='\n')length++;
    if(length==_size)return;
    length++;
    // Do not split a diagnostic line across a radio-critical interval. Reserve
    // 5 ms plus >20% of the nominal 115200-baud drain time for scheduling jitter.
    if(remaining>1000000)remaining=1000000;
    if(length>(remaining-5000)*9/1000)return;
    while(length){
      size_t contiguous=sizeof(_data)-_head;
      size_t n=length<contiguous?length:contiguous;
      size_t written=Serial.write(reinterpret_cast<const uint8_t*>(_data+_head),n);
      if(!written)break;
      _head=(_head+written)%sizeof(_data);_size-=written;length-=written;
    }
    Serial.flush();
  }
  uint32_t dropped() const {return _dropped;}
private:
  char _data[4096]={};
  size_t _head=0,_size=0;
  uint32_t _dropped=0;
};
#endif
