---
name: arm64-dma-debug
license: Apache-2.0
description: "诊断 ARM64 嵌入式 DMA 的数据损坏、地址不可达、缓存一致性、对齐回退和停止回收竞态。适用于 PL330、IDMAC、ADMA2、xHCI 等已有传输路径。"
---

# ARM64 DMA 数据与生命周期排错

先确定故障在数据、地址、完成语义还是生命周期，保存一个可比较的PIO/CPU基线。

- 将CPU指针、物理/总线地址、DMA引擎寻址宽度、descriptor和data区域分别列出。64位CPU不保证DMA能访问所有堆。
- 画buffer所有权变化；按平台cache API定义处理TX clean、RX可见性及descriptor发布，不把相邻CPU脏数据一起失效。
- 检查cache-line对齐、descriptor长度/地址边界、短包及零长语义；不能用指针强转代替地址转换。
- 完成IRQ后仍需读回模式/哈希。记录是否回退PIO、bounce字节数及实际请求大小，避免“协商高速”等于高吞吐。
- 取消/错误时先证明控制器已停止访问，再唤醒waiter、调用callback或释放buffer。证明不了就保留内存并进入受控恢复。

[故障矩阵与案例](references/runbook.md)给出边界组合；[来源](references/sources.md)定位真实实现。

结束时交付最小失败输入、地址/cache/所有权证据及错误后再次传输结果。关闭整个系统cache只能作有边界的诊断对照，不能代替修复。
