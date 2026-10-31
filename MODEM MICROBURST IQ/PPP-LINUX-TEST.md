# PPP over MICROBURST: Linux test

Use two already-flashed modems connected as `/dev/ttyUSB0` and `/dev/ttyUSB1`. Replace these names if needed. PPP configures **115200 baud, 8N1**. Close serial terminals before starting.

## 1. Install the standard Linux tools

Debian/Ubuntu:

```sh
sudo apt install ppp iproute2 iputils-ping
sudo modprobe ppp_async
```

## 2. Create three configuration files

Leave `/etc/ppp/options` unchanged. Create the following files as root.

**/etc/ppp/microburst-common**

```text
115200
local
nocrtscts
xonxoff
noauth
lock
asyncmap a0000
escape 11,13
mtu 1500
mru 1500
noccp
novj
noipv6
nodefaultroute
lcp-echo-interval 0
lcp-echo-failure 0
persist
holdoff 3
maxfail 0
```

Keep `nocrtscts` **before** `xonxoff`: this overrides a system hardware-flow default, then selects software flow control. `asyncmap a0000` asks the peer to escape XON/XOFF; `escape 11,13` also forces their escaping on transmission. Both ends use these settings. PPP handles framing and escaping; no modem firmware change or custom codec is needed.

**/etc/ppp/peers/microburst-a**

```text
file /etc/ppp/microburst-common
/dev/ttyUSB0
10.253.77.1:10.253.77.2
ifname mbpppa
linkname microburst-a
```

**/etc/ppp/peers/microburst-b**

```text
file /etc/ppp/microburst-common
/dev/ttyUSB1
10.253.77.2:10.253.77.1
ifname mbpppb
linkname microburst-b
```

## 3. Start both ends on one Linux computer

Use separate network namespaces so Linux cannot answer the peer IP locally and bypass the radio.

```sh
sudo ip netns add mbpppa
sudo ip netns add mbpppb
sudo ip -n mbpppa link set lo up
sudo ip -n mbpppb link set lo up
sudo ip netns exec mbpppa /usr/sbin/pppd call microburst-a
sudo ip netns exec mbpppb /usr/sbin/pppd call microburst-b

sudo ip -n mbpppa address show mbpppa
sudo ip -n mbpppb address show mbpppb
sudo ip -n mbpppa route get 10.253.77.2
```

Wait for addresses **10.253.77.1** and **10.253.77.2**. The route check must show `dev mbpppa`.

## 4. Measure RTT and packet loss

Run these tests **one at a time**:

```sh
sudo ip netns exec mbpppa ping -n -c 100 -i 1 -W 5 10.253.77.2
sudo ip netns exec mbpppb ping -n -c 50 -i 1 -W 5 10.253.77.1
sudo ip netns exec mbpppa ping -n -c 10 -i 4 -W 15 -s 512 10.253.77.2
```

The first two tests use the default 56-byte ICMP payload. The last uses 512 bytes and wider spacing to limit queue buildup. Read `packet loss` and `rtt min/avg/max/mdev` in each summary. RTT is round-trip time.

For negotiation troubleshooting, add `nodetach debug` to a start command; run each end in a separate terminal.

## Two separate Linux computers

Copy the common file and the appropriate peer file to each computer, adjusting its serial device. Omit all namespace commands. Run `sudo /usr/sbin/pppd call microburst-a` on A and `sudo /usr/sbin/pppd call microburst-b` on B, then ping the opposite IP normally.

## Stop the one-computer test

```sh
sudo poff microburst-a
sudo poff microburst-b
sudo ip netns del mbpppa
sudo ip netns del mbpppb
```

Reference: [pppd options manual](https://ppp.samba.org/pppd.html).
