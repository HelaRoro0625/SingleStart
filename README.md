# SingleStart

防重复启动托盘工具（原生 Win32 版，v0.2.0）。

**单文件可执行，无需安装 .NET**，体积约 300KB，兼容 Win10/Win11。后台托盘常驻，拦截"短时间内的重复启动"。

功能：
- 通知标题/内容可自定义（支持 `{app}` 软件名）
- 启动时通知、其他程序启动时提醒
- 白名单（列表管理，右键或长按删除）、开机自启、进程扫描间隔（20–2000ms）可配置
- 还原默认设置


## 更新日志

### v0.2.0
- 通知标题支持 `{app}` 软件名
- 新增启动时通知、其他程序启动时提醒
- 白名单改为列表管理，右键或长按删除

## 构建（可选）

依赖 MinGW（ucrt64）g++/windres：

```bash
windres --codepage=65001 -O coff app.rc -o app_res.o
g++ -O2 -s -static -municode -mwindows SingleStart.cpp app_res.o -o SingleStart.exe -lcomctl32 -lversion -lcomdlg32 -ldwmapi -lshell32
```


## 关于

由 **DeepSeek V4 Flash** 共同开发。

## 贡献者

- [HelaRoro](https://github.com/HelaRoro0625)
- [claude](https://github.com/claude)
- [deepseek](https://github.com/deepseek)
