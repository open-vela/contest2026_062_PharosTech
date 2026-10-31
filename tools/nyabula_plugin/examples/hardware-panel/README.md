# 硬件小助手

安装后先授予“组件页面”，即可打开插件。四个按钮分别需要 audio.read、usb.access、bluetooth.read、audio.read；撤销权限后再次点击会被 Core 拒绝。

默认图标是 assets/icon.png。换成随包 iconfont：

```json
"icon": { "type": "iconfont", "path": "assets/icons.woff2", "codepoint": "e601" }
```

图标示例为本项目原创，Apache-2.0。PNG 与 WOFF2 均无需联网。此示例不调用麦克风、摄像头或通话操作；清单列出这些权限用于检查授权界面，默认不授予。
