# 协议与错误案例

## 按责任检查

| 项 | 要回答的问题 |
|---|---|
| 身份 | generation谁生成、何时改变；对端重启后哪些句柄失效 |
| 请求 | ID归发起方还是应答方；REQUEST/RESPONSE/EVENT/CANCEL如何关联 |
| 窗口 | offset/length/capacity是否越界；头部是否保留；allocator是否唯一 |
| 生命周期 | callback何时复制数据；CANCEL后迟到结果是否丢弃；谁释放lease |
| 资源 | 最大并发/包大小/队列/超时；超限是否有确定错误 |

## K7成功和反例

4MiB共享区板测 `ok, 1048572 words match`，有Linux写后openvela读的反向证据。9月20日发现上电保留区为0xffffffff，驱动错误地当成其他owner而拒绝claim；固定独占DT保留区重新初始化并最后发布magic后恢复。这个做法只适用于确认独占区域，不能泛化成“见到任何header都覆盖”。

另一项问题是Linux mmap与openvela映射cache属性不一致，重复读取旧数据；用户态改成匹配的writecombine路径。还有镜像内核早于当前驱动源码，trace offset不一致：先确认实际加载二进制版本，不把此问题全归cache。

## NYAMP当前方向案例

services1–8由openvela请求，BLOB service9由Linux请求。ID归发起方，应答回显；generation仍归计算域，Linux的BLOB ID用bit63额外区分。allocator只在Linux，openvela填窗；READ窗口不得碰前4KiB arena头。新generation应清除旧blob和在途摘要。复制这一设计前先确认同样所有权，不照抄服务号和位分配。

## 可执行验证顺序

先host codec roundtrip/畸形长度测试，再双端pattern；最后引入应用数据和取消/重启。bench分别报告RTT、fill、copy、实际传输量，memory fill吞吐不等于RPMsg带宽。对服务卡死使用有界timeout，不能永久等ACK或自动无限重试有副作用请求。

验收样例：同一request ID在服务重启后重用，旧EVENT_FINISH必须不能完成它；对方发来未知RESPONSE不能回RESPONSE形成风暴。
