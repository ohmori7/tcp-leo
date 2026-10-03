# TCP LEO for starlink

## TCP LEO
TCP LEO is TCP congestion control tailored for Low Earth Orbit (LEO) satellite network, in which periodic handover or network reconfiguration occurs.

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
% sudo insmod tcp_leo
% sudo insmod tcp_leo_cubic.ko
% sudo insmod tcp_leo_bbrv1.ko
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

You can change paramters of the timing and duration to stop transmissions in ms.

```
/sys/module/tcp_leo/parameters/leo_handover_start_ms
/sys/module/tcp_leo/parameters/leo_handover_duration_ms
```

## Confirm/change congestion control

```
% sysctl net.ipv4.tcp_allowed_congestion_control
% sudo sysctl net.ipv4.tcp_allowed_congestion_control="reno cubic leo-cubic tcp_leo_bbrv1"
```

## Debug

```
% sudo sh -c 'echo "file ${SRCDIR}/tcp_leo.c:1234 +p" > /sys/kernel/debug/dynamic_debug/control'
```

## More accurate handover countermeasure

In order to accurately resume transmission right after handover,
TCP LEO patch to a kernel, tcp_leo_kernel.patch, should be applied.
Currently, below Linux kernel sources can be patched.

- Ubuntu 24.04.5 LTS (GA) (kernel 6.8.0)
- Google BBR kernel (kernel 6.13.7-bbr)

For BBRv3 kernel, a kernel can be built by:

```
% git clone https://github.com/ohmori7/tcp-leo-bbr
% git fetch origin v3_tcp_leo
% git checkout -b v3_tcp_leo origin/v3_tcp_leo
% cd tcp-leo-bbr
% cp /boot/config-$(uname -r) .config
% make olddefconfig
% echo CONFIG_TCP_CONG_BBR1=m >> .config
% make -j$(nproc) LOCALVERSION=-tcp-leo-bbr3 2>&1 | tee ~/bbr3-build.log
```

If you are using Ubuntu or Debian, it would be better to to make .deb.

```
% make -j$(nproc) bindeb-pkg LOCALVERSION=-tcp-leo-bbr3
```

You can then install:

```
sudo dpkg -i ../*.deb
```

## TODO
- secure boot support (currently, no digital signature)
- BBRv3 support
