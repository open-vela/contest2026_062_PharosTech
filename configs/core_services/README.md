# Core 原生服务回归

该 sim 配置开启 SQLite 产品状态、计时/闹钟、认证 Web 控制和 Eye，使用 hostfs
保存测试数据；不依赖开发板。Agent、蓝牙、AMP 的独立验收不由此配置替代。

在完整 openvela 工作区内，先应用 `patches/apps-wamr-context-default.patch`。
GCC 14 + libc++ 17 的 sim 使用与原工作区一致的兼容参数：

```sh
export CXXFLAGS=-D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS
./build.sh contest2026_062_PharosTech/configs/core_services --cmake
python3 contest2026_062_PharosTech/tools/nyabula_core/tests/product_records_sim.py \
  <生成的nuttx可执行文件> --output /tmp/core-records-proof
```

同目录还有 product_timers、product_alarms、product_web 回归。CI 为该配置运行
这四条真实 sim 测试；它们不调用云模型，不使用真实账号或生产数据库。
