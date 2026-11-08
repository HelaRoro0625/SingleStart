# SingleStart

防重复启动工具。

**单文件可执行**，体积约 300KB，兼容 Win10/Win11。后台托盘常驻，拦截"短时间内的重复启动"。

功能：
- 通知标题/内容可自定义（支持 `{app}` 软件名）
- 拦截重复启动、拦截到重复启动时气泡通知（两个独立开关，默认全开）
- 启动时通知、其他程序启动时提醒（可开关）
- 启动提醒小窗：抗锯齿圆角卡片，弹出时"快速渐显 + 上滑回弹"（点击位置在屏幕顶部时改为下滑）同时播放；同一个软件同时只保留一条提醒（连点、反复点都不会重复弹），不同软件可以同时各显示一条
- 设置页按"基本 / 拦截 / 提醒 / 白名单 / 高级 / 实验性选项"用分组标题加分割线分成几组
- 视觉引导动画：目标软件窗口显示后，提醒移动到窗口位置并放大到窗口大小覆盖其上（放大过程中图标逐渐居中并放大、文字逐渐淡出），随后渐隐消失；即使提醒已经超时消失，目标窗口之后才出现时也会从原位置补播这段动画（默认关闭，开启"其他软件启动时提醒"后才可选）
- 深色模式：设置界面与提醒小窗都跟随系统明暗自动切换；提醒小窗为半透明卡片，圆角/描边全部是逐像素 alpha 抗锯齿（不使用系统模糊材质：材质会从圆角外的透明像素透出来，且分层窗口得不到真模糊）
- 白名单（列表管理，右键或长按删除）、开机自启、进程扫描间隔（20–2000ms）可配置
- 运行模式（实验性选项）：默认轮询；可切换为窗口事件检测，空闲时 CPU 占用更低（不创建窗口的进程无法感知）
- 关于窗口：版本、作者信息与相关链接
- 还原默认设置

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
