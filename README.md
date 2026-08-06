# SingleStart-Lite

防重复启动托盘工具（轻量版）。

与 [SingleStart](https://github.com/HelaRoro0625/SingleStart)（WinUI 3 版）**功能逻辑一致**：
- 后台托盘常驻，拦截"短时间内的重复启动"
- 通知标题/内容可自定义（支持 `{count}` / `{app}`）
- 白名单、开机自启、进程扫描间隔（20–2000ms）可配置
- 还原默认设置

区别：**原生 Win32 界面**，单文件可执行，**无需安装 .NET**，体积约 300KB，兼容 Win10/Win11。

## 构建（可选）

依赖 MinGW（ucrt64）g++/windres：

```bash
windres --no-preprocess -O coff app.rc -o app_res.o
g++ -O2 -s -static -municode -mwindows SingleStart-Lite.cpp app_res.o -o SingleStart-Lite.exe
```

## 注意

两个版本的单实例互斥体同名，**不要同时运行**。

由 HelaRoro 和 DeepSeek V4 Flash 共同开发。
