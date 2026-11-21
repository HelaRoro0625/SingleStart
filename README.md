# SingleStart

防重复启动工具。

**单文件可执行**，体积约 300KB，兼容 Win10/Win11。后台托盘常驻，拦截"短时间内的重复启动"。

功能：
- 拦截重复启动并气泡通知
- 开机自启
- 其他软件启动时弹出提醒小窗
- 视觉引导动画
- 白名单
- 多运行模式：1.快速响应（默认，使用轮询方案，将造成持续少量CPU占用）
              2.低耗能（使用窗口事件检测方案，空闲时CPU占用趋近0%，但提醒的弹出有延迟不创建窗口的进程无法感知）


## 构建（可选）

依赖 MSYS2 MinGW-w64 工具链。64 位用 `ucrt64`，32 位用 `mingw32`。32 位版本兼容 Win7–Win11；64 位版本在 Win7 上需安装 UCRT 更新（KB2999226）。

### 64 位（ucrt64）

```bash
export PATH="/c/msys64/ucrt64/bin:$PATH"
windres --codepage=65001 -O coff app.rc -o app_res.o
g++ -O2 -s -static -municode -mwindows SingleStart.cpp app_res.o -o SingleStart.exe -lcomctl32 -lversion -lcomdlg32 -ldwmapi -luxtheme -lgdiplus -lwinmm -lole32 -lshell32
```

### 32 位（mingw32，需先安装 `pacman -S mingw-w64-i686-gcc`）

```bash
export PATH="/c/msys64/mingw32/bin:$PATH"
windres --codepage=65001 -O coff app.rc -o app_res32.o
g++ -O2 -s -static -municode -mwindows SingleStart.cpp app_res32.o -o SingleStart-32.exe -lcomctl32 -lversion -lcomdlg32 -ldwmapi -luxtheme -lgdiplus -lwinmm -lole32 -lshell32
```


## 关于

由 **DeepSeek V4 Flash** 开发。
