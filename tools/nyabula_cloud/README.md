# 本机运行 Nyabula Cloud

Cloud 是独立服务：K7 主动连接它，浏览器通过它访问设备。K7 不需要开放公网入站端口。Go 服务同时提供账号、设备认领、统计、中继和已构建的 Console 静态页面。

## 启动

在 `tools/nyabula_cloud` 目录构建（需要 Go 1.26；若外层存在无关的 `go.work`，先设置 `$env:GOWORK="off"`）：

```powershell
go test ./...
go build -o nyacloud.exe ./cmd/nyacloud
```

先构建当前候选的 Console：

```powershell
pnpm --dir ../../app/nyabula_web --filter @nyabula/nyalink --filter @nyabula/eye-engine build
pnpm --dir ../../app/nyabula_web --filter @nyabula/console build
```

运行本机联调服务。`-db` 使用独立数据库，避免覆盖已有账号与设备：

```powershell
./nyacloud.exe -listen 0.0.0.0:8088 -db ./local-test.db -console ../../app/nyabula_web/apps/console/dist
```

浏览器打开 `http://127.0.0.1:8088/`。健康检查为 `GET /api/v1/health`，正常返回 `{"status":"ok"}`。仅本机测试时改用 `-listen 127.0.0.1:8088`；板端联调需要监听本机局域网地址或 `0.0.0.0`，防火墙只放行板 IP 到该端口。

上述路径以 `tools/nyabula_cloud` 为工作目录。Console 与 API 同源，无需另开 Vite 服务或填写 API 地址。

## 连接 K7

1. 固件启用 `CONFIG_NYABULA_CORE_CLOUD=y`。在局域网设备面板设置访问密码。
2. 在设备面板「设备 → Cloud」启用中继，地址填 `ws://<电脑局域网 IP>:8088`。公网使用 `wss://`。
3. 在 Cloud 注册/登录账号，用设备面板显示的设备 ID、认领码绑定设备。
4. 打开设备详情，输入**设备访问密码**。它与 Cloud 账号密码不同。通过鉴权后可控制表情和注视点，状态从真机回传。

持有板端 CLI 时也可配置；JSON 文件不含密码：

```json
{"enabled":true,"url":"ws://192.168.31.83:8088"}
```

将文件传到板上后执行：

```sh
nyabula request cloud.config /tmp/cloud-config.json
nyabula request cloud.status
```

`connected=true` 才表示设备已完成 Cloud 注册。认领只建立 Cloud 账号归属，不跳过设备鉴权。设备密码忘记时，在板端执行 `nyabula panel reset-password`，按提示输入两次新密码。

## 外部部署使用 TLS

服务可直接监听 HTTPS/WSS，也可置于支持 WebSocket Upgrade 的 TLS 反向代理后：

```sh
./nyacloud -listen :8443 -db /var/lib/nyabula/cloud.db \
  -console /opt/nyabula/console \
  -tls-cert /etc/nyabula/fullchain.pem -tls-key /etc/nyabula/privkey.pem
```

两个 TLS 参数必须同时提供，最低 TLS 1.2。证书 SAN 必须匹配设备配置中的域名或 IP；反向代理超时须允许持续的 WebSocket 连接。私钥仅留在服务端。

K7 的信任 CA 文件默认为 `/config/nyabula/cloud-ca.pem`，由 `CONFIG_NYABULA_CORE_CLOUD_CA` 指定。部署时配置该服务对应的可信 CA 链，并保证设备时间正确。客户端保持证书链和主机名校验；测试证书只用于隔离联调，不用于公网部署。

## 数据与运行

- SQLite 使用 WAL；数据库及其 WAL/SHM 文件属于同一运行状态。备份时正常停止服务，再复制数据库；不要在运行中只复制 `.db`。
- Ctrl+C 正常退出会断开中继并刷新统计。设备在服务恢复后自动重连，退避上限 32 秒。
- 注销 Cloud 会话会撤销该会话的远程连接；解绑会关闭该账号对设备的连接。设备断线时客户端关闭并等待重连。
- 统计记录在线秒数与转发帧数，不记录帧正文。Cloud 终止两侧 TLS，因此属于可信服务；当前没有端到端加密。
- 当前是开放注册、自部署服务。家庭共享、邮件找回、商城尚未实现；设备认领按单一账号检查。

协议与各字段见 `../../docs/protocol/cloud.md`。本次板测、固件版本和验收证据见项目根目录 `实测日志.md`。

## 许可

Nyabula 自有代码采用 Apache-2.0；第三方依赖遵循各自许可。
