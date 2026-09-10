# Nyabula Core 与 Eye/Display

Core的插件运行时来自已验证提交512956f0。本次在最新团队主线集成Eye视觉子模块，
并连接已合入的Nyabula Display。

## 启用与运行

configs/core_eye是K7独立系统配置，启用真实双屏驱动、LVGL矢量渲染、
QuickJS/WAMR、Core与Eye。它不启用SD host；LCD与SD共用FSPI1引脚，
接屏前按板级接线要求准备。AMP的LCD/IRQ所有权需另外配置。

首次构建前在队伍仓执行：

    python3 app/nyabula/tools/generate_fonts.py --download-fallback

原字体由客户自行下载；缺失时生成Noto Sans回退字形，CI同样走官方源。
字体文件和生成表不提交Git，参见app/nyabula/res/fonts/README.md。

    nyabula_display &
    nycore eye-status
    nycore grant my.plugin core.log
    nycore grant my.plugin ui.notify
    nycore run-package /data/plugins/my.plugin

示例配置的运行状态位于/tmp/nyabula，重启后丢失；可信公钥默认读取
/data/nyabula/trusted-keys.json。先提供签名包与trust store，再运行插件。
程序不会自动绕过缺失的授权或签名。打包工具在tools/nyabula_plugin。

## 插件接口

    import * as ui from "@nyabula/ui";

    await ui.eye({
      action: "eyes.expression",
      params: { expression: "happy", transition_ms: 280 },
      lease_ms: 5000
    });
    await ui.eye({
      action: "eyes.blink", params: { eyes: "left" }
    });
    await ui.notify("Hello Nyabula");

ui.notify显示5秒minimal字幕，UTF-8消息上限47字节；ui.eye使用
原Eye协议的action/params/priority/lease_ms/id字段。未填lease时为5秒，
0表示持续拥有，直到该来源release或Display重启。来源由Core插件ID确定，
不使用JSON里的source。最多32项待处理命令，满队列返回EAGAIN。
lease只参与表情与Scene的来源仲裁；虹膜颜色、环境光和自动眨眼是即时设置，
持续至下一条设置命令或Display重启。凝视使用独立的hold_ms，眨眼为一次性动作。
JSON最多4096字节、嵌套16层；拒绝非有限数字及整数溢出。
core.reset属于管理操作，不开放给此接口；来源只能release自己的请求。

Promise成功表示已入队，nycore eye-status给出渲染线程应用命令后的状态；
不表示面板完成物理扫描。Display未启动返回ENODEV。函数始终在相同
ui.notify权限下运行；撤权会拒绝之后的请求，已入队/显示的场景按lease退出。

支持13类表情、左右/双眼眨眼、凝视、自动眨眼、环境光、异瞳、25类Scene的
show/update/hide，以及按来源release。完整字段契约见app/nyabula/SCENE_SCHEMA.md。

WAMR导入nyabula.ui_eye(ptr,len)->i32，同样传UTF-8 JSON，0表示入队、
负值表示errno。C、Rust与TinyGo SDK均提供对应绑定。命令行可用
nycore eye /path/command.json投递同一协议。

## 生命周期与测试边界

Display持有Eye实例；attach/detach只能在LVGL所有者线程进行。服务锁保护
提交与销毁，销毁后的新请求返回ENODEV。现有Display常驻循环仍不提供强制
kill后的清理保证；需要重启显示时使用系统重启。

UI生产配置关闭mock；本PR不接AI模型、网络offload或新沙箱。测试结果以PR正文
和项目实测日志为准，内存LCD接收端的渲染证明不等于物理面板验证。
