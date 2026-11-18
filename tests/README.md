# 插件测试（plugin tests）

本目录是**仓库内的可复现测试套件**，覆盖「插件基础设施」与 `PluginWord` 的真实渲染，
共 **188 项断言**（`-SkipShots` 185 项）。它替代了以前只存在于 `.workbuddy/tmp`（被 gitignore）里的临时探针 ——
那些探针随机器一起消失，导致同一套验证别人无法跑、也没法回归。

## 为什么这样设计

测试程序（[plugin_tests.cpp](plugin_tests.cpp)）**把主程序源码直接编进来**
（`MainWindow` / `FileManager` / `PluginManager` / `PluginDiagnostics` 都是真的），
插件则通过 `QPluginLoader` 加载**真实构建出来的 DLL**。所以：

- 不需要启动 GUI、不需要点菜单，就能覆盖「选插件 → 建实例 → 送数据 → 渲染」全链路；
- 断言直接检查 `QTextDocument` 的段落、字符格式、表格、图片资源，而不是"看截图猜"；
- 失败信息是「哪一项、期望什么、实际什么」，出问题能直接定位。

## 覆盖了什么

| 前缀 | 内容 |
|---|---|
| `W` | 后缀路由：用户映射优先、插件自描述 `extensions`、大小写/前导点、未知后缀 |
| `W4b` | `.ts` 冲突（MPEG-TS vs TypeScript）按 `extensionPriority` 解析为文本 |
| `W8` | API 版本门禁：当前版本接受、无 `apiVersion` 拒绝、版本不符拒绝、空元数据拒绝 |
| `W9` | 加载失败的插件仍能路由其后缀（sidecar 兜底 → 走「插件不可用」诊断） |
| `W1b` | 启动时对加载失败的插件给出可见提示 |
| `B` / `C` | 真实 docx 渲染：标题层级、混合字符格式、对齐、列表、表格合并、图片、超链接、中文编号 |
| `Z` | 缩放（放大 / 重置） |
| `N` | 异常输入：旧版 `.doc`、`.rtf`、损坏 ZIP、非 Word 压缩包、0 字节；失败后清屏 |
| `M` | 多标签页实例独立性 |
| `D` | 部署自检：清单比对、报告文本、失败登记、补文件后重试恢复、真实构建目录清单校验 |
| `A` | 主程序链路：`PluginManager` 加载、`QMergeFiles` 合并、`resolvePlugin` 三步、挂进 `QTabWidget`、诊断入口存在 |
| `X` | B2 实例契约（`createInstance()` 返回空必须报错，不再退化共享实例）、B4 磁盘空间预检、C5 插件禁用/重启用 + 重新加载 |
| `Y` | C2 数据通道：`PluginFile` 内存/路径两种形态、`sendFile()` 的默认兼容实现（老插件照常收到数据、路径型插件收到原路径）、迁移后的 PluginWord 直接吃 `PluginFile` |
| `V` | 各查看器渲染冒烟：PluginText（.txt、**.html 富文本**）、PluginPicture（.png、**.svg**、**.webp**）、PluginBook（.epub，覆盖共享 ZipReader 的 deflate 路径）、PluginWord（**.odt/OpenDocument** 的段落/表格、**.rtf 子集**）、PluginSheet（**.xlsx 多表 + 合并单元格**、**.ods**）、PluginSlides（**.pptx**、**.odp** 的逐页静态渲染） |
| `F` | 格式识别（`FormatResolver`）：PNG/PDF/OLE2/ZIP 家族（docx/xlsx/odt）按内容判断、内容优先于错误后缀、未知格式有可读建议、不支持格式清单非空 |
| `G` | 合并展开：已有 `.dat` 作为输入时被拆成条目参与合并（不产生嵌套归档）、同目录合并保持短相对路径、不同盘符来源的条目**保留原归属**（`D/testfiles_copy/...` 不会被改挂到另一个盘）、撞名自动改名 |
| `H` | 合并对话框交互：列表为多选整行选择、删除只移除选中项、Delete 键等价于删除按钮；分割对话框支持预填；大小列按数值字节排序；平铺模式一进入就启用列排序（不依赖先点一次"平铺显示"）；树形模式同级排序、目录优先，目录显示子树合计大小与"文件夹"类型；排序（含点列头）不改变节点展开/收起状态 |
| `K` | 保存路径规划：跨盘添加后保存仍保留 `E/...`/`D/...` 盘符段、同盘添加后保留子目录层级、归档内重复路径会被拒绝保存；`archivePath()` 供分割预填 |

素材见 [fixtures/](fixtures)：`basic.docx`（常规排版）、`chinese.docx`（中文样式名 +
中文编号 + 合并单元格）、`legacy.doc`/`sample.rtf`/`broken.docx`/`notword.docx`/`empty.docx`（异常输入）。

## 前置条件

- Windows + Qt（本工程套件：`Qt 6.9.3 / mingw_64` + `Tools/mingw1310_64`）
- 可选：Python + `python-docx` + `Pillow`，仅在**重新生成素材**时需要（素材已入库）

## 一键运行

```powershell
# 完整流程：构建主程序+插件 → 构建测试程序 → 运行
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-plugin-tests.ps1

# 复用已有构建目录（例如 Qt Creator 的影子构建），跳过主程序构建
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-plugin-tests.ps1 `
    -AppBuildDir build\Desktop_Qt_6_9_3_MinGW_64_bit_Debug -SkipProjectBuild

# 只跑逻辑用例，不出截图
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-plugin-tests.ps1 -SkipShots

# 改了 docx 素材排版后重新生成 fixtures
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-plugin-tests.ps1 -RegenerateFixtures
```

参数：

| 参数 | 说明 |
|---|---|
| `-QtBin <dir>` | Qt 的 `bin` 目录（含 `qmake.exe`）；默认取 `$env:QTDIR\bin` 或本机套件路径 |
| `-MingwBin <dir>` | MinGW 的 `bin` 目录（含 `mingw32-make.exe`）；默认由 Qt 目录推断 |
| `-AppBuildDir <dir>` | 主程序影子构建根目录（含 `FileWrapper/`、`Plugins/`）；默认 `tests/build/app` |
| `-FixturesDir <dir>` | 素材目录；默认 `tests/fixtures` |
| `-SkipProjectBuild` | 不构建主程序，直接复用 `-AppBuildDir` |
| `-SkipShots` | 不输出渲染截图 |
| `-RegenerateFixtures` | 先跑 `fixtures/make_fixtures.py` 重新生成素材 |
| `-Python <exe>` | Python 可执行文件路径（配合 `-RegenerateFixtures`） |

产物（均被 gitignore）：

```
tests/build/app/                   主程序影子构建（含 plugins/ 与部署产物）
tests/build/plugin_tests/debug/    测试程序 + 测试时用的 plugins/ 副本
tests/build/logs/plugin_tests.log  完整断言日志（UTF-8）
tests/build/shots/*.png            渲染截图（basic / chinese / legacy_doc）
```

退出码：`0` = 全通过；非 `0` = 有失败（日志里 `[FAIL]` 行会直接在终端回显）。

## 直接跑测试程序

脚本只是把下面几步串起来，也可以手工执行：

```powershell
mkdir tests\build\plugin_tests; cd tests\build\plugin_tests
qmake ..\..\plugin_tests.pro -spec win32-g++ CONFIG+=debug
mingw32-make -j4
copy ..\..\..\build\Desktop_Qt_6_9_3_MinGW_64_bit_Debug\FileWrapper\debug\plugins debug\plugins -Recurse
.\debug\plugin_tests.exe ..\..\fixtures <程序目录>\plugins .\out.log --shots .\shots --integrate
```

`plugin_tests.exe <fixture-dir> <plugins-dir> <log-file> [--shots <dir>] [--integrate]`

- `--shots <dir>`：额外保存渲染截图（basic / chinese / legacy_doc）
- `--integrate`：追加 A 组（主程序 UI 链路）用例

> 用显式开关而不是位置参数：以前"第 4 个是截图目录、第 5 个是 integrate"，
> 一旦跳过截图，`integrate` 就会被当成目录名，A 组静默不跑（已修复）。

## 加用例

- **新增插件**：在 `fixtures/` 放该格式的素材；在 `plugin_tests.cpp` 里加一组断言
  （参考 `verifyBasicDoc()`）；若是新后缀，顺手加到插件的 `pluginX.json`
  并在 `checkExtensionMapping()` 的表里加一行，确认路由。
  - 插件代码有公共部分：**`Plugins/common/`**（`zipreader`、`pluginbase.h`）。
    新插件一律继承 `PluginBase<自身, MainWindow>` 并只实现 `showFile()`，
    不要再抄构造/析构/`apiVersion`/`createInstance` 这套样板；
    需要共享源码时在 `.pro` 里写 `PLUGIN_SHARED_SOURCES = zipreader`。
  - 渲染冒烟参考 `verifyOtherViewers()`：**从本程序自己的 `plugins/` 目录**
    用 `QPluginLoader` 取模板、再 `createInstance()` 渲染，用完 `delete`；
    **不要**从另一个路径再去加载同一个 DLL（Windows 会当成两个模块，
    卸载其一会让插件侧遗留对象的虚表悬空 → 退出期崩溃，V 组踩过）。
- **改插件接口**：记得 `PLUGIN_API_VERSION +1`（见 `include/plugininterface.h`），
  并同步所有插件 JSON 的 `"apiVersion"`；版本门禁用例 `W8` 会继续守住"旧插件被拒"。
  **当前接口版本 = 3**（C2 引入 `sendFile(const PluginFile &)`）。
- **新插件想吃"零拷贝"通道**：在 `showFile()` 里直接用 `file.memory()`；
  基类的 `sendFile()` 已经把空值/路径/旧通道都适配好了。

## 已知限制

- 不驱动主程序的「载入归档」文件对话框（`QFileDialog` 模态循环在无头环境会把进程卡住）；
  A 组改为直接调用 `QMergeFiles` 信号并用 `QTabWidget::addTab` 覆盖挂载路径。
- 「插件不可用 / 插件诊断」对话框本身是模态 UI，未做点击自动化；其**内容生成逻辑**
  （`D4`）、**失败登记与恢复**（`D5`/`D6`/`D7`）、**入口存在性**（`A8`）、
  **启动提示**（`W1b`）已自动化覆盖。
- 「插件管理」对话框（勾选启用/禁用）同样是模态 UI，未做点击自动化；其底层
  `setPluginEnabled()` / `reloadPlugins()` 与"禁用后不加载但仍路由后缀"的行为由 `X4`/`X5` 覆盖。
- C5 的 `reloadPlugins()` 会**卸载插件 DLL**，只能在没有任何标签页持有实例时调用
  （UI 里点「重新加载插件」会先确认并关闭所有标签页）。测试里把它放在最后执行，也是这个原因。
