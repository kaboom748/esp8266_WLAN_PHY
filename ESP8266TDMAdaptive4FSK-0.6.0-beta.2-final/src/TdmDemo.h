#ifndef ESP8266_TDM_DEMO_H
#define ESP8266_TDM_DEMO_H
#include "ESP8266TDM.h"
#include "TdmLog.h"
extern "C" {
#include "user_interface.h"
}
#ifndef TDM_ROLE
#define TDM_ROLE 1
#endif

RH_ESP8266FSK radio;
ESP8266TDM link(radio);
TdmLog logOutput;
bool initialized=false,automatic=true,reportRequested=true;
uint32_t demoSeq=0,lastReport=0,lastRxReport=0,lastAckReport=0;
char command[64]={},notice[80]={};
uint8_t commandLength=0;
bool commandOverflow=false;

void demoPayload(uint8_t* data,uint8_t role,uint32_t seq){
  data[0]=0xa5;data[1]=role;TdmProtocol::put32(data+2,seq);
  for(uint8_t i=6;i<ESP8266TDM::MAX_PAYLOAD;i++)data[i]=uint8_t(seq*17+i*29)^role;
}

void executeCommand(){
  if(commandOverflow){snprintf(notice,sizeof(notice),"ERROR command_too_long");return;}
  if(!strcmp(command,"/reset")){ESP.restart();return;}
  if(!strcmp(command,"/pause")){link.pause(true);snprintf(notice,sizeof(notice),"PAUSED");return;}
  if(!strcmp(command,"/run")){link.pause(false);snprintf(notice,sizeof(notice),"RUNNING");return;}
  if(!strcmp(command,"/auto on")){automatic=true;snprintf(notice,sizeof(notice),"AUTO on");return;}
  if(!strcmp(command,"/auto off")){automatic=false;snprintf(notice,sizeof(notice),"AUTO off");return;}
  if(!strcmp(command,"/stats")){reportRequested=true;return;}
  if(!strcmp(command,"/iq 0") || !strcmp(command,"/iq 1")){
    radio.setIqMode(command[4]-'0');snprintf(notice,sizeof(notice),"IQMODE %u",radio.iqMode());return;
  }
  if(!strcmp(command,"/gain agc")){
    radio.setReceiveGain(-1);snprintf(notice,sizeof(notice),"GAIN agc");return;
  }
  if(!strncmp(command,"/gain ",6)){
    int rf,bb;char extra;
    if(sscanf(command+6,"%d %d %c",&rf,&bb,&extra)!=2 || rf<0 || rf>6 || bb<0 || bb>7
       || !radio.setReceiveGain(rf,bb))snprintf(notice,sizeof(notice),"ERROR gain_rf_0_to_6_bb_0_to_7");
    else snprintf(notice,sizeof(notice),"GAIN rf=%d bb=%d",rf,bb);
    return;
  }
  if(!strncmp(command,"/limit ",7)){
    char* end=nullptr;long value=strtol(command+7,&end,10);
    if(end==command+7 || *end || value<0 || value>=TdmRate::COUNT)snprintf(notice,sizeof(notice),"ERROR limit_0_to_7");
    else {link.setRateLimit(uint8_t(value));snprintf(notice,sizeof(notice),"LIMIT %ld",value);}
    return;
  }
  if(!strcmp(command,"/recal")){radio.restartReceive(true);snprintf(notice,sizeof(notice),"RECAL");return;}
  if(!strncmp(command,"/send ",6)){
    const char* data=command+6;uint16_t id=0;
    size_t length=strlen(data);
    if(length>ESP8266TDM::MAX_PAYLOAD || !link.send((const uint8_t*)data,length,&id))snprintf(notice,sizeof(notice),"ERROR send_length_or_queue");
    else snprintf(notice,sizeof(notice),"QUEUED id=%u",id);
    return;
  }
  snprintf(notice,sizeof(notice),"ERROR unknown_command");
}

void readCommands(){
  // Keep command parsing bounded between radio polls.
  if(!Serial.available() || radio.mode()==RHGenericDriver::RHModeTx)return;
  char c=Serial.read();
  if(c=='\r')return;
  if(c=='\n'){
    command[commandLength]=0;executeCommand();commandLength=0;commandOverflow=false;
  }else if(commandLength<sizeof(command)-1)command[commandLength++]=c;
  else commandOverflow=true;
}

void report(){
  auto s=link.statistics();auto r=radio.statistics();auto cal=radio.lastCalibration();
  logOutput.printf("TDM role=%u state=%s cycle=%u tx=%lu rx=%lu miss=%lu txFail=%lu ack=%lu delivered=%lu retry=%lu dup=%lu queue=%u resync=%lu f0=%ld f1=%ld corr=%u peerCorr=%u heap=%u t=%lu rate=%u symbolUs=%lu limit=%u up=%lu down=%lu recovery=%lu\n",
    TDM_ROLE,link.stateName(),s.cycle,(unsigned long)s.txCells,(unsigned long)s.rxCells,(unsigned long)s.missed,
    (unsigned long)s.txFailures,(unsigned long)s.acknowledged,(unsigned long)s.delivered,(unsigned long)s.retries,
    (unsigned long)s.duplicates,link.queued(),(unsigned long)s.resyncs,(long)cal.zeroHz,(long)cal.oneHz,
    radio.lastCorrections(),s.peerCorrections,ESP.getFreeHeap(),(unsigned long)millis(),link.rate(),
    (unsigned long)link.bitUs(),link.rateLimit(),(unsigned long)s.rateUps,(unsigned long)s.rateDowns,(unsigned long)s.recoveries);
  logOutput.printf("PHY role=%u sync=%lu crc=%u reject=%lu slow=%lu abort=%lu late=%lu calAge=%lu invalid=%lu full=%lu workUs=%lu samples=%lu maxPoll=%lu\n",
    TDM_ROLE,(unsigned long)r.syncs,radio.rxBad(),(unsigned long)r.correctionRejects,(unsigned long)r.slowPolls,
    (unsigned long)r.txAborts,(unsigned long)r.txLateUs,cal.valid?(unsigned long)(millis()-cal.atMs):0,
    (unsigned long)s.invalid,(unsigned long)s.queueFull,(unsigned long)r.workUs,(unsigned long)r.samples,(unsigned long)r.maxPollUs);
  logOutput.printf("ADAPT role=%u rate=%u next=%u waitMs=%lu good=%u qualityQ4=%u bad=%u losses=%u\n",
    TDM_ROLE,link.rate(),link.rate()+1<TdmRate::COUNT?link.rate()+1:link.rate(),
    (unsigned long)link.probeWaitMs(),link.rateGood(),link.rateQuality(),link.rateBad(),link.rateLossWindow());
  logOutput.printf("IQ role=%u hz=%ld e=%lu dtNs=%lu jitterNs=%lu mode=%u\n",TDM_ROLE,
    (long)r.lastHz,(unsigned long)r.lastEnergy,(unsigned long)r.sampleStepNs,(unsigned long)r.sampleJitterNs,radio.iqMode());
  logOutput.printf("WINDOW role=%u slotUs=1000000 guardUs=20000 fillBits=%lu syncs=%lu phaseUs=%ld frames=%u bytes=%u windows=%lu queuedBytes=%u deliveredBytes=%lu ackBytes=%lu outOfOrder=%lu logDrop=%lu\n",TDM_ROLE,
    (unsigned long)r.fillBits,(unsigned long)s.clockSyncs,(long)s.clockErrorUs,
    s.windowFrames,s.windowBytes,(unsigned long)s.txWindows,unsigned(link.queuedBytes()),
    (unsigned long)s.deliveredBytes,(unsigned long)s.acknowledgedBytes,(unsigned long)s.outOfOrder,
    (unsigned long)logOutput.dropped());
  logOutput.printf("IQ35 role=%u estimates=%lu rawSamples=%lu activeUs=%lu estimatesPerSec=%lu\n",TDM_ROLE,
    (unsigned long)r.samples,(unsigned long)r.iqSamples,(unsigned long)r.workUs,
    r.workUs?(unsigned long)(uint64_t(r.samples)*1000000/r.workUs):0);
  int32_t centers[4]={};bool trained=radio.fourToneCenters(centers);
  logOutput.printf("FSK4 role=%u trained=%u centers=%ld,%ld,%ld,%ld symbolUs=%lu rawBps=%lu dither=off calUs=160000\n",
    TDM_ROLE,trained,(long)centers[0],(long)centers[1],(long)centers[2],(long)centers[3],
    (unsigned long)link.symbolUs(),(unsigned long)link.rawBitRate());
  logOutput.printf("FRAME role=%u crcOk=%lu addressReject=%lu to=%u from=%u len=%u good=%u candidates=%lu drops=%lu\n",TDM_ROLE,
    (unsigned long)r.crcFrames,(unsigned long)r.addressRejects,r.lastTo,r.lastFrom,r.lastLength,radio.rxGood(),
    (unsigned long)r.decodedCandidates,(unsigned long)r.candidateDrops);
  auto rf=radio.rfState();
  logOutput.printf("RF role=%u manual=%u rxStop=%u path=%03x rfWord=%02x bbWord=%03x vga=%u apwr=%u ask=%u txAskField=%u\n",
    TDM_ROLE,rf.manual,rf.rxStopped,rf.path,rf.rfWord,rf.bbWord,(rf.bbWord>>3)&7,rf.apwr,rf.ask,rf.txAskField);
  lastReport=millis();lastRxReport=s.rxCells;lastAckReport=s.acknowledged;reportRequested=false;
}

void setup(){
  Serial.begin(115200);system_update_cpu_freq(160);delay(250);
  link.setRateLimit(5);
  initialized=link.begin(TDM_ROLE==1?ESP8266TDM::MASTER:ESP8266TDM::FOLLOWER);
  Serial.printf("\nTDM_READY version=0.6.0-beta.2-4fsk role=%u chip=%06x session=%08lx cycleUs=%lu payload=%u ok=%u\n",
    TDM_ROLE,ESP.getChipId(),(unsigned long)link.session(),(unsigned long)link.cycleUs(),ESP8266TDM::MAX_PAYLOAD,initialized);
  Serial.flush();
}

void loop(){
  if(!initialized){delay(10);return;}
  link.poll();readCommands();
  if(automatic && !link.paused() && link.queued()<ESP8266TDM::QUEUE_SIZE){
    uint8_t data[ESP8266TDM::MAX_PAYLOAD];demoPayload(data,TDM_ROLE,demoSeq);
    if(link.send(data,sizeof(data)))demoSeq++;
  }
  bool window=link.maintenanceWindow();
  {
    if(notice[0]){logOutput.println(notice);notice[0]=0;}
    ESP8266TDM::Message message;
    // Queue diagnostics now, transmit them only in verified idle time.
    if(link.recv(message)){
      bool valid=true;
      if(message.length==ESP8266TDM::MAX_PAYLOAD && message.data[0]==0xa5){
        uint8_t expected[ESP8266TDM::MAX_PAYLOAD];
        demoPayload(expected,3-TDM_ROLE,TdmProtocol::get32(message.data+2));
        valid=!memcmp(expected,message.data,sizeof(expected));
      }
      char hex[ESP8266TDM::MAX_PAYLOAD*2+1];
      for(uint8_t i=0;i<message.length;i++)snprintf(hex+i*2,3,"%02x",message.data[i]);
      logOutput.printf("RXDATA role=%u id=%u len=%u hex=%s valid=%u\n",TDM_ROLE,message.id,message.length,hex,valid);
    }
    auto s=link.statistics();
    if(window && (reportRequested || millis()-lastReport>4000))report();
    logOutput.service(link.maintenanceUs());
  }
  yield();
}
#endif
