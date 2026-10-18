#!/bin/bash
echo '=== NapCat 的挂载 ==='
docker inspect napcatqq --format '{{range .Mounts}}{{.Source}} -> {{.Destination}}{{"\n"}}{{end}}'
echo '=== shinx-bot 的挂载 ==='
docker inspect shinx-bot --format '{{range .Mounts}}{{.Source}} -> {{.Destination}}{{"\n"}}{{end}}'
echo
echo '=== 两个容器看到的 resource 目录是否一致 ==='
echo -n 'shinx-bot: '; docker exec shinx-bot ls /workspace/resource/nikohug/ 2>&1 | tr '\n' ' '; echo
echo -n 'napcatqq : '; docker exec napcatqq ls /workspace/resource/nikohug/ 2>&1 | tr '\n' ' '; echo
echo
echo '=== NapCat 是否能读到刚生成的图 ==='
docker exec napcatqq ls -la /workspace/resource/download/ 2>&1 | tail -5