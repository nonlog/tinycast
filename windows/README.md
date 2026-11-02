# Tinycast Windows Preview

**Unofficial, partial Windows adaptation of [Tinycast](https://github.com/abue-ammar/tinycast).**
Native C++20 and Win32. No WebView, Electron, browser engine, Node or .NET runtime.
The original macOS app remains unchanged. The Windows port is AGPL-3.0 too.

## 安装与使用

从本仓库的 **Windows 预览版 Release** 下载 `Tinycast-Windows-x64.zip`，核对附带的
`SHA256SUMS.txt` 后解压，运行 `Tinycast.exe`。面向 Windows 11 x64；无需管理员权限。
这是未签名的预览程序，不要关闭系统安全保护，也不要把“可运行”当作所有功能已验收。

发布页同时附带由 CI 根据实际 ZIP 校验值生成的 `tinycast.json`，可交给 Scoop 安装：

```powershell
scoop install https://github.com/nonlog/tinycast/releases/download/windows-v0.1.0-preview.1/tinycast.json
```

使用你现有的 Scoop 安装目录，不要求安装到 C 盘。升级前先从托盘退出程序。
直接从这个版本专属 URL 安装不会替你监控未来版本；后续更新应使用维护中的 Scoop bucket 或新版发布清单。

默认全局快捷键是 **Ctrl+Alt+Space**。关闭面板不会结束进程，退出请使用托盘菜单。
没有安装浏览器运行时，没有自动更新器，也不会默认开启登录启动。

| 操作 | 用法 |
| --- | --- |
| 搜索应用 | 输入应用名称；支持模糊匹配、直接输入中文。暂不支持拼音搜索 |
| 选择与执行 | 上下方向键选择，Enter 执行，双击执行；Ctrl+D 收藏 |
| 返回 | Esc 先清空查询，再返回主界面，最后关闭面板 |
| 设置 | Ctrl+, 或底部齿轮、托盘「设置」 |
| 计算 | 输入 `2+3*4`、`sqrt(81)`、`10kg to lb`、`32 f to c`，Enter 复制结果 |
| 网页搜索 | `g 搜索词` 或 `gh 仓库名`；只在执行后交给默认浏览器 |
| 剪贴板 | 在设置中主动启用；Tab 切换。Enter 粘贴到之前的应用，Ctrl+Enter 只复制 |
| 文件搜索 | 搜索并打开「搜索文件」，输入至少两个字符。只搜索配置的文件夹 |
| 窗口管理 | 搜索「左半屏」「右半屏」「窗口居中」等命令，作用于唤出面板前的窗口 |
| 文本片段和命令 | 设置中打开 `settings.json` 编辑，保存后执行「重新加载设置」 |

界面支持跟随系统、深色、浅色，以及系统语言、简体中文、English。设置界面提供快捷键预设；
更细的快捷键通过 JSON 配置：修饰键 Alt=1、Ctrl=2、Shift=4、Win=8，相加组合；Space 的虚拟键码是32。
快捷键冲突不会静默顶掉其他程序的绑定。

## 配置与隐私

默认配置文件为 `%LOCALAPPDATA%\TinycastWindows\settings.json`。首次启动使用内置默认值，
首次更改设置才保存文件。默认文件搜索目录是桌面、文档和下载。
`settings.example.json` 只是示例，`fileRoots: []` 表示不搜索任何文件夹，不会自动扩展为整块磁盘。

剪贴板历史**默认关闭**。开启后仅记录文本，保存在当前进程内存中，默认最多100项、总计最多8 MiB，
退出后消失，不写入磁盘、不上传、不记录诊断日志。尊重 Windows 剪贴板排除格式，并排除部分常见
密码管理器的进程；**这不保证识别所有密码或敏感文本**，复制敏感信息前应关闭记录。
程序自身复制的结果不会再次进入历史。清空历史不更改当前系统剪贴板。

文件搜索是按需、可取消的文件名扫描，不是完整索引。每次最多扫描2万项、深度8、约1.5秒，
最多返回100项；跳过网络路径、重解析点与离线文件。磁盘系统调用本身仍可能延迟。
找不到文件并不代表文件不存在。应用索引来自开始菜单和打包应用；同名应用去重，
不会自动枚举磁盘上所有便携版 `.exe`。

快捷链接只接受 HTTP(S)，输入使用 UTF-8 URL 编码。文本片段支持 `{query}`、`{clipboard}`、
`{date}`、`{time}`，不递归展开。`{clipboard}` 仅在主动执行片段时读取。
自定义命令使用独立 `executable` 和 `arguments` 字段，**每次执行前确认**；不对查询字符串做 shell 拼接。
JSON 配置为明文，不要在其中存放 API 密钥、密码或其他凭据。
格式损坏或版本不支持的配置文件不会被覆盖，修复并重新加载后才能保存设置。

窗口移动和模拟粘贴可能被 Windows 的权限隔离阻止，尤其是管理员应用。
不要为了这个预览版将整个启动器提权；改用复制、手动粘贴。

## Feature scope

This is a usable first Windows implementation, **not full Tinycast parity**. It does not run Raycast
extensions, AI/MCP, dictation, calendar integrations, image clipboard/OCR, automatic snippet expansion,
or the full macOS calculator/Spaces/Rooms feature set. See [PORTING.md](PORTING.md).

The UI uses a native Unicode Edit control and an owner-drawn native ListBox, with a result-sized panel
and row-height checks at 125%, 150% and 175% scaling. IME composition is left to
Windows; command keys are suppressed during composition. Automated Unicode round-trip and synthetic
composition checks are not a substitute for testing a real Microsoft Pinyin/Japanese/Korean IME.

## Build and verification

The `Windows native preview` GitHub Actions workflow configures MSVC x64, builds Release with a static
CRT, runs the shared-core tests and isolated platform tests, then starts the native window in dark and
light modes. It packages the executable, license, notice, documentation, SHA-256 and immutable source
commit. It does **not** build macOS. The workflow fails on compiler, test or startup failures.

The build commands, run on Actions:

```powershell
cmake -S windows -B windows/build -A x64 -DBUILD_TESTING=ON
cmake --build windows/build --config Release --parallel 2
ctest --test-dir windows/build -C Release --verbose --output-on-failure
```

`Tinycast.exe --smoke-test --theme dark --capture preview.bmp --diagnostics report.json` uses a temporary
isolated profile, fixture applications, no global hotkey, no tray, no startup registration and no
clipboard listener. It exits with a failing code if its assertions fail. Diagnostics contain memory
counters and control status only, not clipboard content, search queries or user file paths.
Those memory numbers describe a fixture UI, not steady-state usage on a real application index.

## License and origin

The repository's `LICENSE` and `NOTICE.md` apply. This fork retains the upstream history and sources.
Windows contributors adapted Tinycast's shared fuzzy-match tiers and window geometry, reimplemented
the platform services, and added a native Windows interface. This is not endorsed by the upstream
maintainer. Source for a distributed package is identified in its `SOURCE.txt` and `build-info.json`.
