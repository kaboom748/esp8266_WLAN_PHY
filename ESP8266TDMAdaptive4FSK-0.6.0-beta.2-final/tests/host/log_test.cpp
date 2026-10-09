#include <cassert>
#include "TdmLog.h"
NullSerial Serial;
int main(){
  TdmLog log;
  log.printf("hello %u\n",42);
  log.service(5000);assert(Serial.output.empty());
  log.service(5500);assert(Serial.output.empty());
  Serial.maxWrite=0;log.service(100000);assert(Serial.output.empty());
  Serial.maxWrite=3;log.service(100000);assert(Serial.output=="hello 42\n");
  std::string expected=Serial.output;
  for(unsigned i=0;i<1200;i++){
    log.printf("line %u with text\n",i);
    expected+="line "+std::to_string(i)+" with text\n";
    log.service(100000);
  }
  assert(Serial.output==expected && !log.dropped());
  std::string huge(900,'x');log.println(huge.c_str());assert(log.dropped()==1);
  puts("PASS logger: complete-line idle budget, zero/short writes, ring wrap, overflow reporting");
}
