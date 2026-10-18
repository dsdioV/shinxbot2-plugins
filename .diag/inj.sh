#!/bin/bash
echo '=== 监听端口 ==='
ss -lntp 2>/dev/null | grep -E '3001|3002|20017|1997' || netstat -lntp 2>/dev/null | grep -E '3001|3002'
echo
echo '=== 时区对照 ==='
echo -n 'host UTC: '; date -u '+%H:%M:%S'
echo -n 'host CST: '; date '+%H:%M:%S'
echo -n 'container: '; docker exec shinx-bot date '+%H:%M:%S'
echo
echo '=== 注入一条 nikohug（用真实 op 账号）==='
PORT=$(awk '{print $2}' /opt/projects/shinxbot2/config/port.txt)
echo "recv port = $PORT"
NOW=$(date +%s)
BODY=$(printf '{"time":%s,"self_id":3633512131,"post_type":"message","message_type":"private","sub_type":"friend","message_id":%s,"user_id":3046907223,"group_id":0,"message":"nikohug 10000","raw_message":"nikohug 10000","font":0,"sender":{"nickname":"t","sex":"unknown","age":0}}' "$NOW" "$NOW")
echo "BODY=$BODY"
curl -s -o /dev/null -w 'inject http=%{http_code}\n' -X POST "http://127.0.0.1:$PORT" -H 'Content-Type: application/json' -H 'X-Self-ID: 3633512131' -d "$BODY"
sleep 5
echo '=== 容器日志（最近 3 分钟的 UTC）==='
docker logs --since 3m shinx-bot 2>&1 | tail -20