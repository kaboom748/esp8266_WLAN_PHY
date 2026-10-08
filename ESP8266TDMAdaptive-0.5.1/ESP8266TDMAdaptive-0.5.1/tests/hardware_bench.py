#!/usr/bin/env python3
import argparse, json, pathlib, re, time
import serial

parser=argparse.ArgumentParser()
parser.add_argument("--ports",nargs=2,default=["/dev/ttyUSB0","/dev/ttyUSB1"])
parser.add_argument("--cpu",choices=[80,160],type=int,default=160)
parser.add_argument("--bit-us",choices=[1500,2000,3000,4000],type=int,default=3000)
parser.add_argument("--rounds",type=int,default=10)
parser.add_argument("--sizes",nargs="+",type=int,choices=[1,8,32],default=[1,8,32])
parser.add_argument("--cooldown-ms",type=int,default=1500)
parser.add_argument("--output",default="hardware-results.json")
args=parser.parse_args()
ports=[serial.Serial(p,115200,timeout=0) for p in args.ports]
buffers=[b"",b""]
events=[]
started=time.monotonic()
def command(board,text):
    ports[board].write(text.encode("ascii"))
def collect(seconds,until=None):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        for board,port in enumerate(ports):
            buffers[board]+=port.read(port.in_waiting or 1)
            while b"\n" in buffers[board]:
                line,buffers[board]=buffers[board].split(b"\n",1)
                text=line.decode("ascii",errors="replace").strip()
                if text:
                    event={"t":round(time.monotonic()-started,3),"board":board,"text":text}
                    events.append(event)
                    if "\ufffd" not in text: print(json.dumps(event),flush=True)
                    if until is not None and board==until and text.startswith("RESULT "):
                        return event
        time.sleep(.001)
    return None
trials=[]
try:
    collect(2)
    rate={1500:"k",2000:"f",3000:"n",4000:"v"}[args.bit_us]
    cpu="8" if args.cpu==80 else "6"
    command(0,"1"+cpu+rate+"lc")
    command(1,"2"+cpu+rate+"lc")
    collect(5)
    command(0,"t");command(1,"t");collect(.2)
    for size in args.sizes:
        command(0,{1:"x",8:"y",32:"z"}[size])
        command(1,{1:"x",8:"y",32:"z"}[size])
        collect(.1)
        for _ in range(args.rounds):
            for sender in [0,1]:
                offset=len(events)
                command(sender,"s")
                result=collect(35,until=sender)
                collect(args.cooldown_ms/1000)
                record={"sender":sender,"size":size,"result":result,"confirmed":False,"valid_receives":0}
                if result:
                    match=re.search(r"seq=(\d+) len=(\d+) ok=(\d+) ms=(\d+) retry=(\d+)",result["text"])
                    if match:
                        seq,length,ok,ms,retries=map(int,match.groups())
                        record.update(seq=seq,ok=ok,ms=ms,retries=retries)
                        received=[e for e in events[offset:] if e["board"]==1-sender
                                  and re.match(r"RX seq="+str(seq)+r" len="+str(size)+r" valid=1 ",e["text"])]
                        record["valid_receives"]=len(received)
                        record["confirmed"]=bool(ok and len(received)==1 and length==size)
                trials.append(record)
    command(0,"t");command(1,"t");collect(.2)
finally:
    command(0,"q");command(1,"q");collect(.3)
    for port in ports: port.close()
    summary={"parameters":vars(args),"trials":trials,"events":events,
             "confirmed":sum(t["confirmed"] for t in trials),
             "total":len(trials),"duplicates":sum(max(0,t["valid_receives"]-1) for t in trials),
             "resets":sum("WDT reset" in e["text"] or "Exception (" in e["text"] for e in events)}
    pathlib.Path(args.output).write_text(json.dumps(summary,indent=2)+"\n")
    print("SUMMARY "+json.dumps({k:summary[k] for k in ["confirmed","total","duplicates","resets"]}),flush=True)
if summary["confirmed"]!=summary["total"] or summary["duplicates"] or summary["resets"]:
    raise SystemExit(1)
