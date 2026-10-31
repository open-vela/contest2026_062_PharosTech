# Nyabula Core 与 Eye/Display

Core的插件运行时来自已验证提交512956f0。本次在最新团队主线集成Eye视觉子模块，
并连接已合入的Nyabula Display。

## 启用与运行

configs/core_eye是K7独立系统配置，启用真实双屏驱动、LVGL矢量渲染、
QuickJS/WAMR、Core与Eye。它不启用SD host；LCD与SD共用FSPI1引脚，
接屏前按板级接线要求准备。AMP的LCD/IRQ所有权需另外配置。

真机使用面板GPIO TE中断：屏0为`/dev/gpio1`，屏1为`/dev/gpio2`。
软件TE仅保留在模拟器验证配置中，模拟器调度结果不代表真机帧率。
屏幕控制器选择GC9B72，使用ST77916-family驱动中的GC9B72初始化序列，
不能沿用该驱动默认的ST77916控制器配置。

首次构建前在队伍仓执行：

    python3 app/nyabula/tools/generate_fonts.py --download-fallback

原字体由客户自行下载；缺失时生成Noto Sans回退字形，CI同样走官方源。
字体文件和生成表不提交Git，参见app/nyabula/res/fonts/README.md。

    nyabula_eye &
    nycore eye-status
    nycore grant my.plugin core.log
    nycore grant my.plugin ui.notify
    nycore run-package /data/plugins/my.plugin

`nyabula_eye`是Eye模块自己的产品入口，通过Display公共API使用显示服务。
`nyabula_display`命令仍是队友的双屏demo，不承担Eye初始化；两者不要同时运行。
本PR不改动`app/nyabula_display`目录中的任何文件。

示例配置的运行状态位于/tmp/nyabula，重启后丢失；可信公钥默认读取
/data/nyabula/trusted-keys.json。先提供签名包与trust store，再运行插件。
程序不会自动绕过缺失的授权或签名。打包工具在tools/nyabula_plugin。

## 网页面板服务的自动恢复

产品启动仍使用 `nyabula_web &`。监听循环遇到异常后由服务自身重建监听，
无需额外常驻进程，也不重启系统。现有连接会断开，面板按原流程自动重连。

- accept 的连接中止或临时资源不足：100ms 起退避，最多 1 秒，继续使用监听器。
- 监听器失效或重建失败：1、2、4、8、16、30 秒退避，后续最多每 30 秒尝试一次；
  一次服务稳定运行至少 60 秒后，下次从 1 秒开始。
- `nycore web-stop` 是主动停服：包括退避等待期间都有效，不会自动拉回。
  使用 `nyabula_web &` 再次启动即可；重复启动会被拒绝。
- 恢复复用已有凭据和认证代次，不重新生成 token，不清空记录，也不停止音频和产品任务。
- 显式指定端口、token 文件和 Origin 的运维启动仍保持单次运行，不自动重试。

`dmesg` 中的 `nyabula_web:` 日志记录失败阶段、errno、监听事件和下次等待时间，
不记录凭据。凭据加载错误会直接退出，避免通过重试或重新生成凭据掩盖配置问题。
HTTP 和 WebSocket 发送均抑制 SIGPIPE，客户端断开只结束对应请求。

这里恢复的是监听循环的可报告错误；强制杀死整个服务任务或内存破坏不属于这个循环的恢复范围。

在指定构建机运行故障回归：

```sh
python3 tools/nyabula_core/tests/web_recovery_test.py app/nyabula_core
python3 tools/nyabula_core/tests/http_disconnect_test.py app/nyabula_core
python3 tools/nyabula_core/tests/product_web_sim.py /path/to/nuttx --output /tmp/web-sim-evidence
```

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
不表示面板完成物理扫描。Eye产品入口未启动返回ENODEV。函数始终在相同
ui.notify权限下运行；撤权会拒绝之后的请求，已入队/显示的场景按lease退出。

支持13类表情、左右/双眼眨眼、凝视、自动眨眼、环境光、异瞳、25类Scene的
show/update/hide，以及按来源release。完整字段契约见app/nyabula/SCENE_SCHEMA.md。

WAMR导入nyabula.ui_eye(ptr,len)->i32，同样传UTF-8 JSON，0表示入队、
负值表示errno。C、Rust与TinyGo SDK均提供对应绑定。命令行可用
nycore eye /path/command.json投递同一协议。

## 生命周期与测试边界

Eye产品入口持有Eye实例；attach/detach只能在LVGL所有者线程进行。服务锁保护
提交与销毁，销毁后的新请求返回ENODEV。常驻渲染循环仍不提供强制
kill后的清理保证；需要重启显示时使用系统重启。

UI生产配置关闭mock；本PR不接AI模型、网络offload或新沙箱。测试结果以PR正文
和项目实测日志为准，内存LCD接收端的渲染证明不等于物理面板验证。
