# 哔哩哔哩 · 声核效果器

在 Windows 声核的通道效果器链中播放哔哩哔哩视频。可按关键词搜索，或用视频详情链接、BV 号和 AV 号直接打开。播放器支持进度定位、全屏和弹幕开关。

## 使用

- 在声核的在线效果器中安装「哔哩哔哩」，或从 [Releases](https://github.com/audcore/audcore-fx-bilibili/releases) 下载并打开 `bilibili-v版本号.fx.audcore`。
- 在通道内添加「哔哩哔哩」，通过搜索框或搜索结果选取视频。支持视频拖放的新版声核也可以直接把链接、BV 号或 AV 号拖到通道效果器列表。
- 视频声音从效果器插入点进入通道，受后续效果器和通道路由处理。所选视频编号随效果器状态保存。关闭播放器窗口结束本次播放。
- 效果器需要 x64 Windows、Edge WebView2 Runtime 和支持 D3D11 视频解码的显卡。目前播放范围为哔哩哔哩提供公开 MP4 播放流的视频。

## 网页源预览

运行 `node scripts/preview-bilibili.mjs`，用浏览器或 OBS 浏览器源打开 `http://127.0.0.1:38690/bilibili/`。预览使用真实搜索、弹幕和视频，声音由浏览器播放。

## 构建与验证

需要 Visual Studio C++ 工具、Windows SDK、CMake、WebView2 SDK 和 Python。可用 `AUDCORE_WEBVIEW2_SDK` 指定 SDK 的 `build/native` 目录。

运行 `pwsh -NoProfile -File scripts/build-local-bilibili.ps1`。脚本串行构建独立包，检查 mono/stereo PCM、视频编号保存与恢复，并从实际视频验证 Media Foundation 提供 D3D11 解码表面。产物在 `build/package/`。

- [声核官网](https://audcore.utae.cn/)
- [声核下载、使用说明和反馈](https://github.com/audcore/audcore-document)
- [效果器问题反馈](https://github.com/audcore/audcore-fx-bilibili/issues)
