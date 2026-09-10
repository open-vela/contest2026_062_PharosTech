# Nyabula 双屏显示对接

## 当前模型

Eye Engine 每个 tick 产生固定顺序的两份逻辑帧：

```text
frames[NYABULA_EYE_LEFT]  -> 猫的左眼
frames[NYABULA_EYE_RIGHT] -> 猫的右眼
```

左右是产品物理语义，不是 framebuffer 的临时编号。板级走线若交换了屏幕片选，
应在显示适配层修正映射，不能交换 Eye Engine 的左右语义。

renderer 始终创建两个独立 `360×360` canvas 和两组时间双缓冲：

```text
canvas[NYABULA_EYE_LEFT]  -> 左眼 360×360 surface，中心 (180, 180)
canvas[NYABULA_EYE_RIGHT] -> 右眼 360×360 surface，中心 (180, 180)
```

当两个 parent 相同时，sim 仅把左、右 canvas 放在宿主 `720×360` framebuffer 的
`x=0` 和 `x=360` 处并排展示；这不会合并其 draw buffer。真实双屏应分别传入两个
display root，每个 canvas 均位于自身 display 的 `(0, 0)`。

## 与真实双屏实现的边界

队友的两个独立 `360×360` display buffer 与 Eye Engine 的双 surface 一一对应。
推荐保持以下职责边界：

- Eye Engine：表情、凝视、眨眼、Scene、转场和单调时间线；
- LVGL renderer：用 Vector Graphic API + ThorVG 把两份逻辑帧分别绘制到左右 eye surface；
- board/display：buffer 生命周期、cache flush、脏区、FSPI 排队和屏幕片选；
- Nyabula Core：来源、优先级、lease 和状态真源，不接触 framebuffer。

禁止在显示链路中生成 `720×360` 合成帧后再按行拆分。对接时调用
`nyabula_eye_engine_create_dual(left_root, right_root)`，使左右 renderer surface 直接
进入各自 display 的 flush queue。物理左右、颜色格式和 flush 应由固定测试图独立
验证，不能通过交换 Eye Engine 的左右语义补偿板级走线。

两块屏即使共享 FSPI、必须串行传输，也应保留独立的 flush completion。动画时间
继续从同一个 LVGL 单调 tick 采样，不能按某块屏的传输完成次数推进，否则总线
拥塞时左右眼会逐渐失步。

## 渲染后端与内存

- 基础圆、虹膜渐变、眼皮、天气图形和 Scene 装饰均生成 LVGL vector path，由
  ThorVG software backend 负责覆盖采样与抗锯齿；renderer 不直接读写像素。
- SVG 资源保留原始二次/三次 Bézier 命令，最终图标不经过预栅格化或轮廓折线化。
- minimal Scene 退出时使用一块可复用的透明离屏 surface，以 `DST_IN` 合成真实
  眼皮蒙版；睡眠 ZZZ 则在提交矢量线段前做眼皮几何裁剪。
- 左右眼各有两个 `360×360 ARGB8888` 时间页，离屏蒙版另占一个同尺寸页面，当前
  总量约 2.47 MiB。ARGB8888 用于保持渐变与蒙版质量；切换 RGB565 必须先做真机
  视觉对比，不能作为默认的性能捷径。
- sim 的宿主窗口尺寸仍可为 `720×360`，但该尺寸只存在于显示后端，不出现在
  renderer 的 draw buffer 分配与眼睛坐标中。

## 眼球与视线语义

眼睛按球体旋转建模，不能把整张虹膜圆盘平移：

- 物理眼球圆和虹膜底色始终以单眼 `(180, 180)` 为中心，并留出固定圆框；
- 视线只驱动瞳孔、高光和虹膜放射纹理；绿色眼底及其径向照明完全固定；
- 放射纹理内端跟随瞳孔较多、外端跟随较少，形成球面旋转视差；
- 接近边缘时保持 Demo 定义的瞳孔尺寸，并约束完整轮廓和纹理端点留在眼球内；
- 眼皮最后覆盖眼球内容，但不能依赖眼皮掩盖越过物理圆框的错误几何。

当前 ThorVG 版本不能把 `LV_VECTOR_BLEND_DST_IN` 当作通用实时圆形 clip 使用；实测
会把源圆直接绘成白色。眼球边界因此使用固定底盘和几何约束实现；现有 Scene 离屏
蒙版是不同的合成路径，仍须在每次后端升级后单独回归，不能据此推广到主画布。

## 双眼同帧优化

左右 display、canvas 和双缓冲始终独立。renderer 仅在两份 frame 逐字节一致、没有
Scene、斜眼皮、斗鸡眼、睡眠粒子、星星或眩晕左右相位时，光栅化左眼一次，再用
`lv_draw_buf_copy()` 复制到右眼当前页。异瞳、单眼眨眼、左右构图和任何不对称状态
会自动回退为双独立光栅。该优化复用已经确定完全相同的像素结果，不改变几何、
抗锯齿、渐变、动画 tick 或两块物理屏的 flush 生命周期。

## 建议的合并顺序

1. 先让板级双屏驱动独立显示纯色和固定测试图，确认左右与色序；
2. 为两个 LVGL display 分别建立 `360×360` root 和 flush completion；
3. 调用双 parent 创建接口，验证左右眼中心、Scene、蒙版和同步时间线；
4. 最后接入脏区和共享 FSPI flush queue，测量稳定帧率与最坏传输延迟。

双屏驱动不应复制表情状态机或 Scene 绘制代码；Eye Engine 也不应包含 ST77916、
FSPI、片选或 DMA 细节。这样双方分支只会在 renderer 创建接口和 APP 启动代码
发生小范围对接，不会在产品逻辑上互相覆盖。

## 当前性能策略

- 固定眼底、48 根虹膜放射纹理和外圈使用两个 `360×360 ARGB8888` LRU 页面缓存；
  视线、颜色、发光或缩放变化会严格失效，约增加 `0.99 MiB` 内存；
- 左右像素可证明一致时只光栅一次，但两块屏仍保留独立 canvas、双缓冲和 flush；
- 普通待机眼在同一底图键下只恢复上一帧瞳孔与高光覆盖的矩形；共享右眼复制
  新旧动态矩形的并集。眨眼、Scene、旋转、复杂表情和底图键变化自动回退全页恢复；
- Vector descriptor、高质量 path、Scene 蒙版 path 均跨帧复用，提交后的 task 所有权仍由
  LVGL 接管；
- 完全睁眼时不提交不可见眼皮，固定外圈随底图缓存；睡眠眼皮采样仅在该眼确有活跃
  ZZZ 粒子时生成；
- 虹膜线角度和半径只在 renderer 创建时计算，圆弧使用 LVGL 原生 Bézier arc；
- 爱心隐式曲线在 renderer 创建时求解一次；眩晕螺旋以单条连续 vector path 提交，
  不再逐线段创建独立 stroke；
- 指数插值尾差小于 `0.00001` 时吸附目标，稳定且不依赖时间的整帧可停止重绘。

2026-08-17 的 x86_64 ThorVG sim 最终回归：待机约 `1.89–3.85 ms/双眼帧`，
其中 buffer copy 约 `0.50–0.74 ms`，常见窗口维持 `62.0–62.6 fps`；25 类 Scene
完整轮播约 `1.51–6.61 ms/双眼帧`、`62.2–62.6 fps`，静态单眼复用率最高 `72%`。
13 类表情完整轮播中最重的复杂窗口约 `9.79–10.87 ms/双眼帧`，眩晕窗口由约
`53.8 fps` 提升至约 `57.2 fps`。
这是软件渲染基准，不可直接等同 RK3576 真机数据。多子路径合并和“首次位置不建底图
缓存”均经 A/B 证明为负优化并已撤回。
