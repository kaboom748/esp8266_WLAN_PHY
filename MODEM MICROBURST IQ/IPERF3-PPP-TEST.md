# iperf3 speed tests over the MICROBURST PPP link

First establish PPP using [PPP-LINUX-TEST.md](PPP-LINUX-TEST.md). Keep its configuration unchanged: 115200 baud, XON/XOFF and PPP escaping. No firmware change or additional PPP configuration file is needed.

| End | Network namespace / interface | PPP address |
|---|---|---|
| A | `mbpppa` | `10.253.77.1` |
| B | `mbpppb` | `10.253.77.2` |

Stop continuous pings and run **one speed test at a time**.

## 1. Install iperf3

Debian/Ubuntu:

```sh
sudo apt install iperf3
```

If asked to run a system-wide iperf3 daemon, choose **No**. Start the server inside B's namespace instead.

## 2. Start the server on B

Check whether a server is already listening on port 5201:

```sh
sudo ip netns exec mbpppb ss -ltnp
```

Reuse an existing server bound to `10.253.77.2:5201`. Otherwise, run this in a separate terminal and leave it open:

```sh
sudo ip netns exec mbpppb iperf3 -s -B 10.253.77.2
```

## 3. Measure TCP throughput

From a normal Linux terminal, test **A → B**:

```sh
sudo ip netns exec mbpppa iperf3 -c 10.253.77.2 -t 60 -i 10 -f k -M 256 -l 256 -w 2K
```

Then test **B → A** by adding `-R`:

```sh
sudo ip netns exec mbpppa iperf3 -c 10.253.77.2 -t 60 -i 10 -f k -M 256 -l 256 -w 2K -R
```

If you are already inside the shell opened with `sudo ip netns exec mbpppa bash`, omit the namespace prefix. For example:

```sh
iperf3 -c 10.253.77.2 -t 60 -i 10 -f k -M 256 -l 256 -w 2K
```

| Option | Purpose |
|---|---|
| `-t 60` | Run the data test for 60 seconds |
| `-i 10` | Report every 10 seconds |
| `-f k` | Display throughput in kbit/s |
| `-M 256` | Request a TCP MSS of 256 bytes |
| `-l 256` | Use 256-byte application writes |
| `-w 2K` | Request small socket buffers; Linux adjusts their actual sizes |
| `-R` | Send test data from B to A |

Small writes and socket buffers reduce the initial queue buildup on this slow link. This TCP test has **no explicit bitrate cap**; TCP still controls its sending rate.

## 4. Read the result

Use the final **receiver** bitrate as the delivered application throughput. The sender can initially report a much larger rate because data entered local buffers before crossing the radio. Zero-byte client intervals do not necessarily mean the receiver stopped receiving.

`Retr` counts TCP retransmitted segments; it is not a radio packet-loss percentage. The serial setting of 115200 baud is not the IP throughput.

One measured 30-second run using the bounded-buffer settings delivered **8.41 KBytes**, reported **2.01 kbit/s** at the receiver over 34.20 seconds, and had **1 TCP retransmission**. This is an example, not a guaranteed rate. The receiver may finish after the requested test duration while queued data drains.

## 5. Optional: UDP loss and jitter at a controlled rate

With the same server running, start at **1 kbit/s** with 128-byte datagrams:

```sh
sudo ip netns exec mbpppa iperf3 -c 10.253.77.2 -u -b 1K -l 128 -t 60 -i 10 -f k
```

Read the receiver's `Lost/Total Datagrams` and `Jitter`. This checks delivery at the requested rate; it does not measure maximum capacity. Increase `-b` gradually for a load test. Always specify a low starting rate: iperf3's default UDP rate is much higher than this link can carry.

## Stop

A client exits automatically; **Ctrl+C** cancels it. Use **Ctrl+C** in the foreground server terminal to stop that server. PPP remains connected.

For two separate Linux computers, run the server on B and the client on A, omitting the namespace prefixes.

Reference: [Official iperf3 options](https://software.es.net/iperf/invoking.html).
