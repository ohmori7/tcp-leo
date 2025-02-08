# TCP LEO for starlink

## How to use
- install developer tools.
```
sudo apt install build-essential linux-headers-$(uname -r) gcc
```
- build kernel module.
```
% cd $(THIS_DIRECTORY)
% make
% sudo insmod leo_cubic.ko
% sudo sysctl -w net.ipv4.tcp_allowed_congestion_control="reno cubic leo-cubic"
```

## Globally apply TCP LEO

```
% sudo sysctl -w net.ipv4.tcp_congestion_control="leo-cubic"
```

## Apply only for your application

You need to specify the congestion control algorithm in your application by `setsockopt()' like this:

```
setsockopt(socket, IPPROTO_TCP, TCP_CONGESTION, "leo-cubic", strlen("leo-cubic"))
```

## Handover duration paramters

You can change paramters of duration to stop transmissions in ms.

```
/sys/module/tcp_leo_cubic/parameters/starlink_handover_start_ms
/sys/module/tcp_leo_cubic/parameters/starlink_handover_end_ms
```

## Bugs
- secure boot support (currently, no digital signature)
