# TCP LEO for starlink

# How to use

```
% make
% sudo insmod tcp_leo_cubic.ko
% sudo sysctl -w net.ipv4.tcp_allowed_congestion_control="reno cubic tcp-leo-cubic"
```

# Globally change the congestion control algorithm

```
% sudo sysctl -w net.ipv4.tcp_congestion_control="tcp-leo-cubic"
```

# Change the congestion control algorithm in your application

You need to specify the congestion control algorithm in your application by `setsockopt()' like this:

```
setsockopt(socket, IPPROTO_TCP, TCP_CONGESTION, "tcp-leo-cubic", strlen("tcp-leo-cubic"))
```
