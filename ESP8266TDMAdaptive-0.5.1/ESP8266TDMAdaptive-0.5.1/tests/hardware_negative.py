#!/usr/bin/env python3
import json, re, sys, time
import serial
ports=[serial.Serial('/dev/ttyUSB'+str(i),115200,timeout=0) for i in range(2)]
for port in ports:
    port.dtr=False
    port.rts=True
time.sleep(.1)
for port in ports: port.rts=False
buffers=[b'',b'']; events=[]; checks={}; started=time.monotonic()
def command(i,text): ports[i].write(text.encode())
def read(seconds,until=None):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        for i,s in enumerate(ports):
            buffers[i]+=s.read(s.in_waiting or 1)
            while b'\n' in buffers[i]:
                line,buffers[i]=buffers[i].split(b'\n',1)
                text=line.decode('ascii',errors='replace').strip()
                event={'t':round(time.monotonic()-started,3),'board':i,'text':text}
                events.append(event)
                if text and '\ufffd' not in text: print(json.dumps(event),flush=True)
                if until==i and text.startswith('RESULT '): return text
        time.sleep(.001)
    return ''
try:
    read(2);command(0,'16nlc');command(1,'26nlc');read(.2)
    command(0,'zr')
    queued=read(5,until=0)
    read(3.2)
    command(0,'t');command(1,'t');read(.2)
    timing=re.search(r'ms=(\d+)',queued)
    checks['raw_send_returns_before_airtime']=bool(timing and int(timing.group(1))<200)
    checks['raw_32_received']=any(e['board']==1 and e['text'].startswith('RX seq=0 len=32 valid=1') for e in events)
    command(1,'q');command(0,'x');read(.2)
    command(0,'s');missing=read(35,until=0);read(1.5)
    checks['missing_ack_returns_false']='ok=0' in missing and 'retry=5' in missing
    command(1,'3l');read(.2)
    command(0,'s');wrong=read(35,until=0);read(1.5)
    checks['wrong_recipient_returns_false']='ok=0' in wrong and 'retry=5' in wrong
    command(1,'2l');read(.2)
    command(0,'s');recovered=read(35,until=0);read(1.5)
    checks['link_recovers']='ok=1' in recovered and any(e['board']==1 and e['text'].startswith('RX seq=3 len=1 valid=1') for e in events)
    command(1,'q');read(.2)
    for kind in ['i','h','w']:
        marker=len(events)
        command(0,'zr'+kind);read(1.8)
        checks['tx_cancel_'+kind]=any(e['board']==0 and
            e['text']=='STOP kind='+kind+' result=0 sent=0 gate=0'
            for e in events[marker:])
    command(0,'t');command(1,'t');read(.2)
finally:
    command(0,'q');command(1,'q');read(.3)
    for port in ports:port.close()
    result={'checks':checks,'passed':len(checks)==8 and all(checks.values()),'events':events}
    with open('/home/kaboom/lab_link_fix/negative.json','w') as f:json.dump(result,f,indent=2)
    print('SUMMARY '+json.dumps(checks),flush=True)
if not result['passed']:sys.exit(1)
