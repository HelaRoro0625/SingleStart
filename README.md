# SingleStart

防重复启动托盘工具（原生 Win32 版，v0.1.1）。

**单文件可执行，无需安装 .NET**，体积约 300KB，兼容 Win10/Win11。后台托盘常驻，拦截"短时间内的重复启动"。

功能：
- 通知标题/内容可自定义（支持 `{count}` / `{app}`）
- 白名单、开机自启、进程扫描间隔（20–2000ms）可配置
- 还原默认设置

> 本仓库为**当前主版本**。WinUI 3 重构版（`SingleStart-WinUI`，C#）已移至私有仓库，不再维护。

## 构建（可选）

依赖 MinGW（ucrt64）g++/windres：

```bash
windres -O coff app.rc -o app_res.o
g++ -O2 -s -static -municode -mwindows SingleStart.cpp app_res.o -o SingleStart.exe -lcomctl32
```

## 注意

两个版本的单实例互斥体同名，**不要同时运行**。

## 关于

由 **HelaRoro** 和 **DeepSeek V4 Flash** 共同开发。

## 贡献者

- [HelaRoro](https://github.com/HelaRoro0625)
- [claude](https://github.com/claude)
- [deepseek](https://github.com/deepseek)
