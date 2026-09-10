# Nyabula Wasm 插件 SDK

Eye命令共用`ui.notify`权限：C使用`nyabula_ui_eye(json, length)`，Rust使用
`eye(json)`，TinyGo使用`Eye(json)`，底层导入为`nyabula.ui_eye(ptr,len)->i32`。
0表示命令已入队，不表示面板已扫描；负值为errno。
JSON字段与生命周期边界见[Core说明](../../app/nyabula_core/README.md)。

ABI 版本为 1。C、Rust、TinyGo 共用 WAMR 的 `nyabula` import module，接口分别
位于 `sdk/nyabula_wasm.h`、`sdk/nyabula_wasm.rs` 和
`sdk/nyabula_tinygo.go`。

插件必须导出无参数、无返回值的 `ny_on_start`；可选导出
`ny_on_event(ptr, len)`与`ny_on_stop()`。TinyGo reactor 还会导出
`_initialize`，Core 会在`ny_on_start`前通过同一执行预算调用一次。

## C

使用 openvela manifest 的 clang-wasm：

```sh
clang --target=wasm32 -ffreestanding -nostdlib -O1 \
  -I/path/to/sdk -Wl,--no-entry -Wl,--export-memory \
  -Wl,--max-memory=1048576 main.c -o main.wasm
```

## Rust

已验证 Rust 1.96.0 与`wasm32-unknown-unknown`。Rust 当前会产生新版 Wasm
特性，WAMR 2.1.0 的 legacy Make 解析器不能直接加载；必须用 Binaryen 统一
降级和优化：

```sh
rustc --edition 2021 --target wasm32-unknown-unknown \
  --crate-type cdylib -C panic=abort -C opt-level=s \
  main.rs -o main.raw.wasm
wasm-opt main.raw.wasm --mvp-features -Oz --strip-debug -o main.wasm
```

## TinyGo

已验证 TinyGo 0.41.1、Go 1.25.12 与 Binaryen 131。使用 reactor 模式；禁止
默认 asyncify 调度，否则会引入 Core 未提供的`env.tinygo_rewind`和
`env.tinygo_launch`：

```sh
tinygo build -target=wasm-unknown -buildmode=c-shared \
  -scheduler=none -gc=leaking -no-debug -o main.wasm .
```

Windows 手工安装 TinyGo 时还需提供`WASMOPT`环境变量。TinyGo 0.41.1 在
Go 1.26 下存在工具链兼容问题，当前交付固定 Go 1.25.x。

## 打包

manifest 使用`"runtime":"wamr"`和`"apiVersion":1`。生成模块后统一执行：

```sh
python nyabula_plugin.py test PROJECT
python nyabula_plugin.py pack PROJECT --private-key KEY --key-id ID
python nyabula_plugin.py verify PACKAGE --trust-store TRUST \
  --require-signature
```

SDK 和命令只定义开发接口；测试私钥、sample、编译产物及`.nya`包不进入仓库。
