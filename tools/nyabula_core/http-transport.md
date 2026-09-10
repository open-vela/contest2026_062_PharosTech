# HTTP传输层当前状态

`NYABULA_CORE_HTTP`构建`ny_http_open/step/cancel/close`，复用NuttX webclient的HTTP解析、非阻塞socket状态机及abort。传输层本身不鉴权；QuickJS的`network.request(url)`已通过Broker接入真实传输，返回`Promise<{status:number, body:string}>`，body按UTF-8文本构造并保留NUL。WAMR也已通过token/响应回调接入同一Broker，见`wasm-async.md`。该配置优先于mock，不会把失败的真实请求退回echo。DNS已接入；TLS仍未完成，不能把此切片视为完整Network provider。

## 已实现的边界

- 一个请求由创建它的Broker线程独占；跨线程step/cancel/close返回EPERM。其他线程必须向所有者投递取消消息，不能直接操作socket上下文。
- URL小于256字节、只允许可打印ASCII，不接受空白及CR/LF注入；接受数字IPv4或合法DNS主机名的HTTP地址。显式端口必须为1–65535的纯数字，拒绝上游解析器的uint16溢出回绕及尾随字符；拒绝fragment。
- 域名解析复用NuttX DNS client。上游webclient在进入非阻塞socket状态机前仍同步解析DNS，因此请求deadline不能中断解析阶段。HTTPS继续拒绝。
- 收到Location头立即拒绝，不能重定向到未经批准的域名或端点。
- 响应体最多4096字节，超限EFBIG，错误不交付部分正文。正文按长度返回，允许嵌入NUL，不保证字符串终止。
- 单请求总墙钟期限1–60000 ms，由每次step检查；所有者必须继续调度step。pending返回EAGAIN，终态保留到close。
- 只有webclient_perform返回EAGAIN后才可abort。终态已由webclient释放内部状态，禁止再次abort；未开始的请求取消也不调用abort。
- timeout/cancel关闭真实连接；完成后cancel不改写已保存结果。close释放请求内存。

## 待接入，不缩减最终目标

1. Broker入口已增加创建与推进鉴权、权限代次、不可恢复的撤权状态；每插件最多8个请求，全局最多MAX_PLUGINS×8个，额度在close释放。每个传输请求固定有界分配，因此原生请求内存也受请求数限制。QuickJS绑定每次推进提供新鲜权限快照；权限刷新唤醒worker，停止唤醒后由所有者关闭请求。
2. QuickJS将请求存于既有pending槽，所属worker每轮最多交付一个HTTP完成；等待时至多10 ms重新推进一次，不依赖后续业务事件。直接run-package也能推进HTTP等待，其他无pump的pending仍返回EINPROGRESS。WAMR提供独立的强类型异步导入与响应回调，支持生命周期defer/complete；跨运行时错误编码统一及跨进程IPC尚未完成，不把传输指针或JSValue跨线程传递。
3. 可取消的异步域名解析、证书验证的TLS、HTTP方法/请求体及受控重定向策略。
4. QuickJS公开URL接口已接入；签名包端到端测试覆盖启动await、后台未await请求、撤权、停止和直接运行。完整生产API仍需请求体/方法、二进制响应与跨运行时一致的错误编码。

`ny_broker_http_*`仅供可信适配层使用。输入client和权限代次不是认证凭据；未来IPC服务必须从经过认证的连接会话取得身份与权限，不能接受插件自报。错误身份不能读取或撤销其他请求，其他线程也不能释放所有者请求。完成后撤权同样禁止再次取回正文。

目前Make/CMake的NuttX TCP回环测试覆盖含NUL的200响应、Location拒绝、4100字节正文拒绝、超时、主动取消、重复取消和跨线程误用。服务端在超时和取消后观察到EOF。测试是实际NuttX socket/webclient链路，不是Network mock；它也不证明真实网卡、DNS或TLS已通过。
