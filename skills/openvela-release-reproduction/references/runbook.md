# 复现清单及失败案例

## 版本记录

在已有repo工作区运行只读记录：

```sh
repo manifest -r -o resolved-manifest.xml
git -C contest2026_062_PharosTech rev-parse HEAD
readlink -f vendor/rockchip/chips
readlink -f packages/demos/contest2026_062_nyabula_core
```

后两项仅在相应linkfile存在时使用。manifest不含dirty修改/第三方blob，需另有补丁和输入清单。公开资料删去凭据，但不能手改原始官方AI日志后当真记录提交。

## 四种“通过”

| 结果 | 实际证明 |
|---|---|
| static library中有符号 | 该对象编译了，不保证进最终ELF |
| 完整链接退出0 | 此工作树/依赖能构建，不保证新工作区复现 |
| 公共CI零固件占位通过 | 布局和编译，不保证无线runtime可用 |
| 板子打印新版本 | 部分运行身份；还要目标功能和镜像hash/槽对应 |

## 已有反例

PR93 head `e2aae2f858e85def2dc1fc4661534faa1685fc15`在2026-09-20快照有板测描述，但CI CMake缺Eye字体，Make报agent_msg_t缺request_id等成员，宿主协议测试误编需要nuttx/config.h的源。此后是否已修需查新head，不沿用旧失败判当前。

同日另有开发树Core linkfile指到兄弟product工作区，改了本树并编译仍未进镜像。用ELF符号/独特标记与真实路径缩小原因，别急着改业务代码。

## 交付材料

配置名、完整依赖版本、实际补丁集、准备命令、工具链、退出码、输出文件与hash、外部资产获取/许可边界、板测范围、已知缺口。打包脚本生成同输入可复现payload时记录SOURCE_DATE_EPOCH；不要求不同工具链的ELF必然逐字节相同。

AI会话日志使用官方采集/校验工具，不能把板测Markdown当AI JSONL，也不能因归集困难伪造CLI日志。Skill源码归档与代码/固件发布是不同动作，用户只授权整理时不上传release。
