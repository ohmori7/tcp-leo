# TCP LEO for starlink

## How to use
- install developer tools.
```
sudo apt install build-essential linux-headers-$(uname -r) gcc
```
- build kernel module.
```
% git clone https://github.com/ohmori7/tcp-leo.git
% cd tcp-leo
% make
% sudo insmode tcp_leo
% sudo insmod tcp_leo_cubic.ko
% sudo insmod tcp_leo_bbrv1.ko
% sudo sysctl -w net.ipv4.tcp_allowed_congestion_control="reno cubic leo-cubic tcp_leo_bbrv1"
```

## Globally apply TCP LEO

```
% sudo sysctl -w net.ipv4.tcp_congestion_control="leo-cubic"
% sudo sysctl -w net.ipv4.tcp_congestion_control="leo-bbrv1"
```

## Apply only for your application

You need to specify the congestion control algorithm in your application by `setsockopt()' like this:

```
setsockopt(socket, IPPROTO_TCP, TCP_CONGESTION, "leo-cubic", strlen("leo-cubic"))
setsockopt(socket, IPPROTO_TCP, TCP_CONGESTION, "leo-bbrv1", strlen("leo-bbrv1"))
```

## Handover duration paramters

You can change paramters of duration to stop transmissions in ms.

```
/sys/module/tcp_leo/parameters/leo_handover_start_ms
/sys/module/tcp_leo/parameters/leo_handover_end_ms
```

## TODO
- secure boot support (currently, no digital signature)
- BBRv3 support
