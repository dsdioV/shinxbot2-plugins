#!/bin/bash
docker exec shinx-bot bash -c '
  echo "=== IPv4 强制 ==="
  for i in 1 2; do curl -4 -s -o /dev/null -w "  v4 total=%{time_total}s ip=%{remote_ip}\n" "https://q1.qlogo.cn/g?b=qq&nk=10000&s=640"; done
  echo "=== IPv6 强制（若不通会卡很久）==="
  timeout 25 curl -6 -s -o /dev/null -w "  v6 total=%{time_total}s ip=%{remote_ip}\n" "https://q1.qlogo.cn/g?b=qq&nk=10000&s=640" || echo "  v6 FAILED/超时(exit=$?)"
  echo "=== 默认（不指定）==="
  for i in 1 2; do curl -s -o /dev/null -w "  auto total=%{time_total}s ip=%{remote_ip}\n" "https://q1.qlogo.cn/g?b=qq&nk=10000&s=640"; done
'