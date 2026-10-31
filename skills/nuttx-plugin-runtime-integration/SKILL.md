---
name: nuttx-plugin-runtime-integration
license: Apache-2.0
description: "将现有 QuickJS/WAMR 插件运行时接入 NuttX 产品，处理 provider/Broker、签名授权、异步完成、版本槽和持久化。用于可信本地插件集成，不承诺任意恶意插件的强隔离。"
---

# NuttX 插件运行时产品接入

先确认复用的运行时版本、产品provider、线程所有者和存储后端，不重新造一套JavaScript/Wasm框架。

1. 对同一功能先接最小provider，再让JS与Wasm调用相同Broker合同；设备能力缺失返回明确错误，不用生产mock填成功。
2. 验签、signer/version撤销、插件权限与每次调用授权分别检查；文件已签名不自动有设备控制权。
3. 异步provider仅保存插件身份/token，不跨线程持有JSContext/JSValue。完成队列按generation交回所属worker，停止/撤权/迟到结果要明确处理。
4. 区分包内候选自测目录和已安装插件共享数据目录；晋级current/last-good的状态要用目标存储可保证的事务。
5. 覆盖配额、取消、并发上限、重启恢复与损坏数据库；sim hostfs通过后还要单独检验真实FAT/flush。

[集成与存储边界](references/runbook.md)、[源码及SDK](references/sources.md)。交付同一签名包跨runtime的真实行为，不把同地址空间Broker称为OS级隔离。
