# VDPlus

Virtual Desktop（Android 串流客户端）的 **LSPosed / libxposed** 增强模块：界面汉化、字体净化、声音控制与码率扩展。

> 适用于 Android（arm64-v8a）+ LSPosed（libxposed API 101 / 102），目标应用 `VirtualDesktop.Android`。

## 功能

- **界面汉化** — 内置词典，支持导入自定义 JSON 词典（覆盖内置）。
- **字体净化** — 中文文本自动替换为字形完整的 CJK 字体，避免缺字方框。
- **禁用背景音乐** — 未连接时不再自动播放背景音乐。
- **禁用性能提示音** — 7 种提示音可逐个选择（长按「禁用性能提示音」）。
- **扩展码率范围** — 放大桌面 / VR 码率档位（长按「扩展码率范围」设置倍率 0–5）。
- **解除自动测速码率限制** — 测速完成后解除带宽上限并重新下发设置。

## 环境要求

| 项目 | 要求 |
| --- | --- |
| 系统 | Android 10+（minSdk 29） |
| 架构 | arm64-v8a |
| 框架 | LSPosed（libxposed API 101 / 102） |
| 目标应用 | Virtual Desktop（`VirtualDesktop.Android`） |

## 安装

1. 从 Releases 下载 `app-release.apk` 并安装。
2. 在 LSPosed 中启用 **VDPlus**，作用域勾选 **Virtual Desktop**。
3. 强制停止并重新启动 Virtual Desktop。

## 使用

底部导航包含两个页面：

- **首页** — 模块激活状态、Virtual Desktop 版本、模块版本与词典信息。
- **选项**
  - 字典：使用导入词典 / 导入词典文件 / 恢复默认词典
  - 声音：禁用背景音乐 / 禁用性能提示音（长按选择提示音）
  - 码率：扩展码率范围（长按设置倍率）/ 解除自动测速码率限制

设置即时生效，无需重启应用；码率相关修改会在下次测速完成后应用。

## 构建

需要 JDK 17 与 Android SDK（compileSdk 34、NDK 27.0.12077973、CMake）。

```bash
export JAVA_HOME=/path/to/jdk-17
./gradlew :app:assembleRelease
# 产物: app/build/outputs/apk/release/app-release.apk
```

- Gradle 8.11.1（wrapper）、AGP 8.7.3。
- `local.properties` 需包含 `sdk.dir=...`（该文件不入库）。

### 签名

Release 签名优先读取环境变量，其次读取根目录 `signing.properties`（两者都不入库）：

```properties
KEYSTORE_FILE=/absolute/path/to/release.keystore
KEYSTORE_PASSWORD=...
KEY_ALIAS=...
KEY_PASSWORD=...
```

未配置时 Release 产物不签名（仅用于本地调试）。CI 使用仓库 Secrets 注入同名变量。

## 自动发布

`.github/workflows/release.yml` 在推送到 `main` / `master` 或手动触发后自动编译，并创建 / 更新 Release（标签 `v<versionName>`，当前 `v1.0`）。

## 项目结构

```
app/src/main/cpp/native.cpp                            native 钩子（Mono 内联 hook）
app/src/main/java/org/ghitori/vdplus/MainModule.java   libxposed 入口
app/src/main/java/org/ghitori/vdplus/MainActivity.java Material 3 界面
app/src/main/res/                                      主题 / 布局 / 图标
app/src/main/resources/META-INF/xposed/                libxposed 注册
app/libs/                                              libxposed api / service jar
```

## 说明

- 模块通过内联 hook 修改运行中的 Mono 方法，不修改 Virtual Desktop 安装包。
- `mono_ldstr_checked` 真实 ABI 为 `(ptr, u32, ptr, ...)`，hook 必须以 `void*` 透传 `x0`–`x3`。

## License

[MIT](LICENSE) © ghitori
