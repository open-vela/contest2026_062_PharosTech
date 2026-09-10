# QuickJS 异步完成契约

## 所有权

插件所属worker调用`ny_plugin_async_begin_authorized()`，提供本次操作需要的权限位，获得Promise和64位token。token包含runtime generation和request ID；每个runtime最多8个pending请求，request ID耗尽后拒绝继续分配。

provider保留插件ID和token，不保留JSContext、JSValue或runtime指针。任务上下文完成入口为：

```c
ny_scheduler_complete(id, token, status, payload, length);
```

该入口在锁内复制最多`CONFIG_NYABULA_CORE_EVENT_SIZE`字节。返回0只表示入队；不代表插件已接受结果。队列独立于用户事件队列，容量为8。队列已满返回EAGAIN；排队中的重复token返回EALREADY；旧实例返回ESTALE；停止中的实例返回ECANCELED。调用者不得在ISR中使用此接口。

worker调用`ny_plugin_async_deliver()`，在所属线程中创建JS值并完成Promise。当前成功结果为按长度构造的字符串，失败结果为负errno整数。Network等更高层绑定应自行定义序列化格式，不能跨线程传递JS对象。直接从其他线程调用runtime的begin/complete/deliver会返回EPERM。

## 权限与生命周期

请求记录所需权限和授权代次。交付时复核两者；期间刷新授权会使旧的受权限保护请求返回EACCES，包括撤销后再次授权的情况。该检查阻止旧结果进入插件，不替代provider实际操作的取消机制。

onStart/onEvent/onStop返回thenable时，运行时等待其最终结果。scheduler中的worker可在等待期间处理完成队列。`cpuMsPerEvent`限制一次生命周期中累计的执行段，等待完成队列时暂停计费，继续执行只使用剩余额度；Promise链不能靠反复等待重置额度。执行段按单调时钟墙钟计量，包含同步宿主调用和被抢占时间，不等同于操作系统线程CPU时间。

`NYABULA_CORE_ASYNC_TIMEOUT_MS`另外限制整个生命周期的墙钟时间，默认5000 ms，允许100–60000 ms；包括初始执行、provider等待和后续回调。达到期限停止等待，忙循环也受此总期限约束。等待超时返回ETIMEDOUT；QuickJS中断的执行可返回EFAULT。没有scheduler pump的同步CLI遇到仍pending的生命周期返回EINPROGRESS，不把它当作成功或允许包晋级。

生命周期完成回调绑定独立代次，旧resolver不能完成后续生命周期。完成消息不重置累计执行额度或总期限；普通后台完成处理有自己的执行预算。

## 停止

停止标志与唤醒独立于用户队列容量。停止后拒绝新完成消息并丢弃尚未执行的用户事件。等待异步结果的worker会被唤醒，启动与停止调用者通过`start_consumed`交接清理所有权，避免启动调用者尚未读取结果时slot已销毁。

正常取消期间拒绝新请求，有CPU/job预算；即使Error分配失败也释放pending宿主引用。最终destroy不执行插件回调。QuickJS HTTP请求已在pending槽中持有Broker句柄，完成/取消/destroy均由所属线程关闭；HTTP参数错误也由pump延后交付，允许调用方先挂上catch。WAMR使用独立强类型异步ABI，见`wasm-async.md`；模块顶层异步加载和跨进程IPC尚未接入本契约。

## 验证范围

本地测试在NuttX sim中重编译全部Core源文件，复用已构建的sim内核和QuickJS/WAMR依赖归档。覆盖延迟完成、复制后原buffer被修改、重复/迟到/超长完成、跨线程JS调用、撤权再授权、生命周期超时、满用户队列停止及启动中停止。测试程序位于工作区临时目录，不随产品提交。

本契约目前仅覆盖同一Core进程内的QuickJS worker；不代表插件间MMU隔离或真实硬件provider已完成。
