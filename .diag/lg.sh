#!/bin/bash
echo '=== bot 日志文件位置 ==='
ls -la /opt/projects/shinxbot2/log/ 2>/dev/null | tail -5
echo '=== 最近的 bot 日志内容 ==='
tail -40 /opt/projects/shinxbot2/log/*.log 2>/dev/null | tail -40
echo
echo '=== 容器 stdout 是否有缓冲 ==='
docker logs shinx-bot 2>&1 | wc -l