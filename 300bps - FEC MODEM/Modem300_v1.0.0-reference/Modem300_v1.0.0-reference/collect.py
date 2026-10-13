"""Read the diagnostic firmware without changing DTR/RTS or sending commands."""
import argparse
import json
import select
import time
from pathlib import Path
import serial

p=argparse.ArgumentParser()
p.add_argument("--seconds",type=float,default=48)
p.add_argument("--output",required=True)
p.add_argument("--baud",type=int,default=115200)
a=p.parse_args()
ports=[]
records=[]
buffers=[b"",b""]
try:
    for name in ("/dev/ttyUSB1","/dev/ttyUSB0"):
        s=serial.Serial(port=None,baudrate=a.baud,timeout=0,exclusive=True)
        s.dtr=False;s.rts=False;s.port=name;s.open();ports.append(s)
    start=time.monotonic()
    while time.monotonic()-start<a.seconds:
        readable,_,_=select.select(ports,[],[],.2)
        for s in readable:
            i=ports.index(s);buffers[i]+=s.read(s.in_waiting or 1)
            while b"\n" in buffers[i]:
                line,buffers[i]=buffers[i].split(b"\n",1)
                text=line.decode("ascii",errors="replace").strip()
                record={"seconds":round(time.monotonic()-start,3),"port":s.port,"text":text}
                records.append(record)
                if "FAST" in text:
                    print(json.dumps(record),flush=True)
    Path(a.output).write_text(json.dumps(records,indent=2),encoding="utf-8")
finally:
    for s in ports:s.close()
