#!/bin/bash
# 观察一次真实 nikohug 期间，各容器的 CPU 与网络状态
echo '=== 当前容器资源 ==='
docker stats --no-stream --format '{{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.NetIO}}' shinx-bot napcatqq
echo
echo '=== 磁盘 IO（可能是瓶颈）==='
which iostat >/dev/null 2>&1 && iostat -x 1 2 2>/dev/null | tail -20 || echo '(无 iostat)'
echo
echo '=== 挂载点与文件系统 ==='
df -h /opt/projects/shinxbot2 | tail -2
mount | grep -E 'opt|shinxbot' | head -5
echo
echo '=== 服务器整体负载 ==='
uptime; nproc