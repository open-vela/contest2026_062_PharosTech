# 集成与存储边界

## provider只接产品能力

UI/AI/HTTP等provider保持注册接口；设备线程处理硬件，插件worker收到有界结果。runtime generation与request ID共同识别实例，旧实例结果不能唤醒新插件。入队成功只是队列确认，不证明屏幕扫描/网络操作已完成。

Core已有TypeScript、C、Rust、TinyGo SDK与 `.nya` 打包，优先用原工具构建/签名，不改变ABI只为一个示例通过。测试C/Wasm指针长度、JS异常、超时与一次性完成，跨线程不传JS对象。

## 成功证据

M1–M4在NuttX sim完成Make/CMake、签名包、权限、HTTP/DNS及多语言运行；9月10日真实K7 AMP也运行JS/Wasm签名包，10轮短测无净内存增长。这不是长时恶意负载隔离测试。

## 重要反例

- NuttX sim的O_NOFOLLOW链路曾跟随符号链接，需结合lstat/fstat及独占目录。该检查仍不能隔离另一拥有原生写权限的进程。
- 目标VFS的rename覆盖可能先unlink目标，不能照搬“temp+fsync+rename就原子”。Core SQLite事务后端绕开特定问题，但未修复所有调用者。
- SQLite unix-none VFS由唯一Broker锁串行化，不能又允许多个进程打开同一DB。损坏DB返回错误，不静默退回过期文件。
- 候选run-package的数据根与installed版本根不同，候选自测不应改生产共享数据。

## 验收场景

正常包运行、坏签名/撤销版本拒绝；授权前拒绝、授权后一次成功、撤权后新调用拒绝；停止中异步回调不访问旧runtime；配额失败不破坏旧值；晋级/失败恢复后指向可解释版本。真实掉电/介质耐久要单列，不从宿主fsync故障注入推断。

达到用户指定provider集成后停止，不顺便建设插件商店/远程安装服务。
