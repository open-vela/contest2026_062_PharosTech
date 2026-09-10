# Core 的 Eye 子模块

这里保留此前 Eye Engine 的渲染、表情、场景与来源仲裁实现。代码来源为
BeaconCat/contest2026_062_PharosTech 的 beaconcat/nyabula-eye-core，
固定提交16364a3e；不启动旧分支的独立演示主程序或HTTP gateway。

Eye是插件Core的视觉子模块，以库形式构建。app/nyabula_core负责插件生命周期、
权限和SDK；这里的历史nyabula_core.cxx仅负责视觉请求的来源/优先级/lease仲裁，
不是另一个插件运行时。

启用Eye前需选择HAVE_CXX、LIBCXX与LIBCXXABI；K7的core_eye配置已包含这些依赖。

Nyabula Display创建两块真实LCD的独立LVGL screen，调用
nyabula_eye_service_attach(left, right)。每轮先执行nyabula_eye_service_tick()，
再执行现有nyabula_display_task()。只有该线程调用Eye/LVGL；插件线程只复制并
投递有界命令。原有双屏缓冲、TE和QSPI调度仍由Display管理。

公共入口在include/nyabula_eye_service.h；渲染细节与场景字段见
[DISPLAY_INTEGRATION.md](DISPLAY_INTEGRATION.md)和[SCENE_SCHEMA.md](SCENE_SCHEMA.md)。
构建前执行 `python3 app/nyabula/tools/generate_fonts.py --download-fallback`。
有原字体文件就生成原字体字形，没有则回退到官方Noto Sans。字体与生成表不进Git，
完整步骤及许可见[字体准备](res/fonts/README.md)。默认使用编译子集，
不保证覆盖任意中文字形；完整字体动态加载为可选项。
