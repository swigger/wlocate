# WLocate 1.0

Windows x64 原生文件查找工具。`wlocate.exe` 提供 GUI 和后台服务，`locate.exe` 提供命令行；两者共享独立的 C++ 索引。日常搜索无需管理员权限，不依赖 Everything、SwiftList 或 .NET。

两个程序均嵌入 UTF-8 activeCodePage manifest，启动时核验进程代码页为 65001，要求 Windows 10 1903 或更新系统。编译器使用 `/utf-8`；窄字符串、索引、IPC 和重定向文本统一为 UTF-8，在 Windows 窗口、Shell 和 W API 边界转换 UTF-16。它不会更改系统区域设置或父终端代码页。

绿色放大镜图标覆盖 EXE、窗口、任务栏、托盘和开始菜单，包含 16–256 像素共九种尺寸。可编辑矢量稿在 `assets/wlocate.svg`，运行 `tools/Build-Icon.ps1` 可重新生成 ICO。

## 本机安装与使用

- GUI / 服务程序：`C:\Program Files\WLocate\wlocate.exe`。
- 命令行：`D:\tools\bin\locate.exe`；同目录的 `wlocate.exe` 可直接启动界面。
- 开始菜单：WLocate。后台服务：`WLocateService`，自动启动。
- 索引和日志：`C:\ProgramData\WLocate`。界面设置：`%LOCALAPPDATA%\WLocate\settings.ini`。
- 原来的 `locate.old.exe` 保留。此次替换前的 locate 备份路径记录于 `C:\Program Files\WLocate\install-state.json`。

```powershell
locate -n 10 "python*.exe"
locate -i -b python
locate -ic "*.dll"
locate -0 -n 100 "*.cpp"
locate -r '^python.*\.exe$'
locate --query 'ext:exe size:>1mb !path:Windows' --sort size --descending -n 20
locate -S
wlocate "python*.exe"
```

## 已实现的功能

| 部分 | 行为 |
|---|---|
| 建索引 | 自动发现带盘符的本地 NTFS 卷；直接读取 MFT、扩展记录及长文件名硬链接；索引名称、父目录、大小、时间、属性 |
| 实时更新 | USN 日志增量更新，约 250 ms 轮询；创建、删除、改名、移动、硬链接及元数据变化；目录改名不必重写所有后代路径 |
| 持久化 | 原子替换的 SHA-256 校验快照；重启后续读 USN；卷标识或日志缺口不匹配时重建；临时繁忙记录保留旧值并重试 |
| 磁盘变化 | 自动加入新 NTFS 盘、移除拔出的盘；一个卷建索引时仍可查询其他就绪卷 |
| 查询 | Unicode、大小写开关、通配符、PCRE2 JIT 正则、AND/OR/NOT 和分组、常用 Everything 过滤函数 |
| 界面 | 输入即时搜索、类型筛选、名称/完整路径/大小/修改时间/扩展名排序、虚拟列表分页、书签、CSV/文本导出、托盘、自定义全局快捷键、可选登录启动 |
| 文件操作 | 原生 Windows Shell 右键菜单和已安装的扩展；打开、所在文件夹、复制、剪切、删除、属性、重命名、拖出；支持跨目录多选 |
| 命令行 | 服务端限量、计数、NUL 分隔、存在性检查、超时与 Ctrl+C 取消、UTF-8 重定向输出 |

程序自身不提升普通 GUI 的权限。安装服务和重建全盘索引才需要管理员权限；Shell 文件操作遵循当前用户的 Windows 权限和系统确认行为。

## 界面语法

默认忽略大小写、匹配名称。空格为 AND，`|` 为 OR，`!` 为 NOT，`<...>` 分组，双引号包含空格或运算符。

```text
python*.exe
<python | node> ext:exe
"Program Files"
file: ext:cpp;h !path:build
folder: path:D:\work
size:>10mb dm:today
size:1kb..10mb dc:2026-01-01..2026-12-31
attrib:h
case:Python
regex:"^python(?:3)?\.exe$"
```

支持 `file:`、`folder:`、`ext:`、`path:`、`parent:`、`size:`、`dm:`/`datemodified:`、`dc:`/`datecreated:`、`da:`/`dateaccessed:`、`attrib:`/`attributes:`、`case:`、`nocase:`、`regex:`、`ww:`/`wholeword:`。属性字母：`h s r a d l`。日期支持 `YYYY-MM-DD`、`today`、`yesterday`、范围和比较运算；按本地日历边界解释。大小单位使用 1024 倍数。完整路径正则以 `/` 作为路径分隔符。

| 快捷键 | 功能 |
|---|---|
| Ctrl+L / Ctrl+F | 定位搜索框 |
| Ctrl+Alt+L（默认，可修改） | 界面已运行时显示窗口 |
| Enter / Ctrl+Enter / Alt+Enter | 打开 / 所在文件夹 / 属性 |
| Ctrl+C / Ctrl+X / Ctrl+Shift+C | 复制文件 / 剪切文件 / 复制完整路径 |
| F2 / Delete / Shift+Delete | 重命名 / 系统删除 / 系统永久删除 |
| Ctrl+A / F5 | 全选 / 刷新 |
| Ctrl+S / Ctrl+D | 导出全部匹配结果 / 收藏当前查询 |

全局唤出快捷键可在 **工具 → 快捷键设置** 或托盘右键菜单中修改。选择 Ctrl / Alt / Shift / Win 组合与按键，点击“确定”立即生效并保存，下次启动继续使用；也可禁用或恢复默认。新组合被占用、系统不允许注册或设置保存失败时，原快捷键保持可用，窗口内会提示原因。为避免拦截日常输入，普通按键需搭配 Ctrl、Alt 或 Win；功能键可单独使用。程序启动时遇到快捷键冲突会给出托盘提示，仍可通过托盘打开窗口修改。

导出先写临时文件，完成后替换目标；CSV 使用 UTF-8 BOM。界面每两秒刷新；有选中项或正在重命名时暂停自动刷新，以免选中对象移动。搜索框清空可显示全部索引。列宽、窗口大小、查询开关及最多 30 条书签会保存。

## 命令行兼容范围

`locate --help` 列出完整选项。支持 Linux locate 常用参数 `-n/-l`、`-i`、`-b`、`-w`、`-A/-a`、`-c`、`-0`、`-e`、`-E`、`-r`、`--regex`、`-S` 和 `--`。

这是 Windows 索引客户端，不读取 GNU/mlocate/plocate 数据库，未实现 `updatedb`、`--database` 或全部 GNU 扩展。默认区分大小写；普通文字按完整路径子串匹配，未带分隔符的通配符和正则按名称匹配，`-w` 强制匹配完整路径，`-b` 强制名称匹配。多个模式默认 OR，`-A` 改为 AND；`--query` 使用界面语法并默认忽略大小写。正则语法为 PCRE2。

无排序的 `-n` 在后端找到足够条目即停止；结果顺序不保证稳定。需要确定顺序时加 `--sort`。`-c -n 10` 的计数最多为 10。退出码：0 有结果，1 无结果，2 错误，130 被取消。索引尚未全部就绪时可能返回部分结果，同时在 stderr 提示并以 2 退出。

## 编译、安装、回退

使用 MSVC、Windows SDK、CMake 3.25+；本机验证环境为 VS 2026、MSVC 19.50、CMake 4.2.3。依赖源已固定在 `third_party/pcre2`，需要其 SLJIT 子模块。

```powershell
.\Build.ps1
# 以管理员身份运行；在这台电脑可以使用 psuac：
psuac -w powershell.exe -NoProfile -ExecutionPolicy Bypass -File D:\work\gitprojs\swiftlist-locate\wlocate\Install.ps1
```

源码构建输出在 `build\bin\Release`。发布包内可直接运行其 `Install.ps1`，或指定 `-BuildDirectory`；`-CliDirectory` 指定命令行目录，默认 `D:\tools\bin`。`-StageOnly` 仅安装服务端及 Program Files 内的程序，不替换 PATH 中的 locate。更新前先退出 WLocate 界面。

`wlocate --install-service` 在提升权限后会将服务程序安装到 Program Files；不会让 SYSTEM 服务从普通用户可写的工具目录运行。安装目录及数据目录仅允许管理员/SYSTEM 写入。

需要回退命令行时，将 `install-state.json` 中 `previous_locate` 对应的文件复制回 `D:\tools\bin\locate.exe`。原始 Everything 客户端 `D:\tools\bin\locate.old.exe` 也一直保留。需要删除服务时，以管理员运行 `"C:\Program Files\WLocate\wlocate.exe" --uninstall-service`。此操作保留索引、用户设置和二进制文件，便于重新安装。

## 验证与性能

`tests/tests.cpp` 覆盖查询语法、Unicode、范围、正则边界、硬链接、路径/分页排序、取消、元数据缓存、MFT 解析、校验快照、压缩及部分卷可用。

`tests/integration.py` 使用工作区内独立 UUID 目录测试实时更新、30 个硬链接跨 MFT 扩展记录、Unicode 长路径、500 文件突发创建、元数据和清理。`tests/protocol.py` 测试畸形请求、60 次并发查询、2 万条输出、超时及未提升权限的重建拒绝。`tests/restart.ps1` 和 `tests/hotplug.ps1` 需要管理员权限；后者只格式化自己新建的临时 VHD，不选择物理磁盘。`wlocate_ui_tests.exe` 创建自己的窗口和测试文件，验证筛选、虚拟列表、跨目录原生菜单及 Unicode 重命名，并保存渲染图。

性能数据与测量口径见 `PERFORMANCE.md`，原始记录在源码目录的 `test-output`。旧版包装器不支持 `-n`；本次 `*.exe` 对比中旧版仅输出 9999 条，而原生版找到 12286 条，因此不能把它的限量/大范围输出直接当作等价性能基准。

## 当前边界

本版交付了本地 NTFS 文件名搜索的完整常用流程，并未实现 Everything 的全部功能，也不保证所有查询都达到其性能。目前不支持无盘符卷、ReFS/FAT/exFAT/网络共享的文件夹索引、内容搜索、ETP/HTTP 服务、Everything 插件/IPC 兼容接口、高级预览及所有搜索函数；未知函数会明确报错。目录大小不递归统计，重解析点目标不会展开为额外的别名路径。仅测试 Windows x64。

第一次全盘扫描、数百万条结果的首次全量排序仍可能需要数秒；首次建索引更久。快照缓存后的启动和常用限量名称查询要快得多。索引中的文件名可被本机经过身份验证的用户查询，和文件内容读取权限是两回事；文件打开和更改仍由 Windows ACL 控制。暂时无法读取的记录会显示为统计中的 `pending` 并重试。

许可证和参考出处见 `THIRD-PARTY-NOTICES.md` 与 `licenses`。
