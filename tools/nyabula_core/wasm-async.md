# WAMR异步ABI与生命周期

新增ABI复用HTTP Broker，不另建网络协议栈。HTTP启用时，旧`network_request`同步回显导入返回ENOTSUP，避免误把mock当作真实网络。旧void生命周期导出仍兼容。

## 导入与导出

| 名称 | Wasm签名 | 语义 |
|---|---|---|
| nyabula.lifecycle_defer | ()→i64 | 当前生命周期进入等待，返回正token；每轮只能defer一次 |
| nyabula.lifecycle_complete | (i64,i32)→i32 | 完成对应等待，result=0成功、负errno失败；重复或旧token返回ESTALE |
| nyabula.http_request | (i32,i32)→i64 | URL地址/长度；成功返回请求token，立即失败返回负errno |
| nyabula.http_cancel | (i64)→i32 | 关闭对应请求，不再交付回调；旧token返回ESTALE |
| ny_on_response（模块导出） | (i64,i32,i32,i32,i32)→void | token、结果、HTTP状态、body地址、body长度 |

`http_request`要求响应导出的签名精确匹配，验证通过后才创建请求；URL从已校验线性内存复制，禁止嵌入NUL。正文原始字节仅在回调期间有效，由Core分配和释放；需要长期保存时由插件复制，禁止释放Core借出的地址。错误不提供正文。C声明见`tools/nyabula_plugin/sdk/nyabula_wasm.h`。

## Token、授权与所有权

token高31位为不回绕的实例代次，低32位区分请求（奇数）和生命周期（偶数）；计数耗尽返回EOVERFLOW，不复用。token必须保留完整64位，不能跨实例保存使用。

每实例最多8个HTTP请求，并继续受Broker全局额度约束。权限刷新更新代次并唤醒worker，交付前复核，撤权结果不得带旧正文。所有Wasm调用、HTTP推进及内存复制均在所属worker执行；stop/destroy由该所有者关闭请求，停止后不交付响应回调。IPC认证和独立地址空间尚未实现，当前原生入口不是面向不可信进程的认证接口。

## 生命周期等待与时限

未defer的旧模块在导出返回后完成。defer后，返回并不代表成功；Core继续推进响应，直到complete、总时限或停止。onStart等待未成功时，不得报告启动成功或晋级候选。onEvent同样支持defer；onStop不允许再defer或创建请求。

响应回调即使提前调用complete，仍必须正常返回；回调随后陷入死循环会被watchdog终止，整轮失败。一次生命周期累计消耗其Wasm执行预算，provider等待不补充预算；总墙钟时间使用`NYABULA_CORE_ASYNC_TIMEOUT_MS`，默认5秒。计量包括Wasm执行中的同步宿主调用，非线程纯CPU时间。

## 已验证与未完成

签名WAT模块及使用C头、由工程自带clang-wasm编译的模块均通过实际NuttX TCP回环请求。覆盖响应签名错误拒绝、token类型混用/重复complete拒绝、缺权、撤权、停止、直接runtime推进、未complete超时、complete后回调忙循环超时，以及超时后QuickJS邻居继续处理事件。

仍需补齐DNS/TLS、UI/AI真实provider、跨运行时统一错误编码、Rust/TinyGo构建验证、认证IPC和MMU隔离。实例化阶段（包括Wasm start section）及初始线性内存超预算还需单独审计；当前事件watchdog测试不证明这些入口安全。不能据此宣称M1–M4全部完成。
