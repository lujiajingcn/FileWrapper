# FileWrapper 图标源与生成流程

界面图标（工具栏 / 菜单 / 对话框）不再手工存多套 PNG，而是：

```
IconMCP（https://iconmcp.com，MIT）
      │  按“含义”搜索、取 SVG
      ▼
resources/icons/svg/<slot>.svg       ← 字形源文件（currentColor，16 格网格）
      │  tools/icons/build-iconset.mjs
      ▼
resources/icons/<slot>_16…64.png     ← Qt 资源实际加载的 PNG
resources/icons/appicon_16…256.png + appicon.ico
resources/resources.qrc              ← 由脚本重新生成，不再手改
```

## 常用命令

```powershell
# 重新生成全部 PNG / ICO / qrc（字形已存在时完全离线）
node tools/icons/build-iconset.mjs

# 某只字形要换：改 iconset.json 里的 iconmcp 名字，删掉 svg/<slot>.svg 再跑上面命令
```

`svg2png.exe` 是 QtSvg 写的小工具，仓库里不提交可执行文件。构建方式二选一：```powershell
# 方式一：qmake
cd tools/icons/svg2png/.build
qmake ..\svg2png.pro -spec win32-g++ "CONFIG+=release"
mingw32-make -f Makefile.Release

# 方式二：直接 g++（本机沙箱里 qmake 的编译器探测会被 pipe 限制卡住时用）
g++ -std=c++17 -O2 -fexceptions -mthreads -DUNICODE -D_UNICODE -DWIN32 `
  -I"E:/V_C/Qt/6.9.3/mingw_64/include" -I"E:/V_C/Qt/6.9.3/mingw_64/include/QtCore" `
  -I"E:/V_C/Qt/6.9.3/mingw_64/include/QtGui" -I"E:/V_C/Qt/6.9.3/mingw_64/include/QtSvg" `
  -I"E:/V_C/Qt/6.9.3/mingw_64/mkspecs/win32-g++" main.cpp -o .build/svg2png.exe `
  -L"E:/V_C/Qt/6.9.3/mingw_64/lib" -lQt6Svg -lQt6Gui -lQt6Core -lQt6EntryPoint `
  -lmingw32 -lshell32 -luser32 -lkernel32 -lgdi32 -lole32 -loleaut32 -luuid `
  -lws2_32 -lwinmm -lversion -lshlwapi -ldwmapi -lsetupapi -lcfgmgr32 -lwtsapi32 -luserenv
```

运行 `svg2png.exe` 时需要 Qt 的 bin 在 `PATH` 里（Qt6Svg/Gui/Core DLL），
并设置 `QT_PLUGIN_PATH` 指向 `...\mingw_64\plugins`（offscreen 平台插件）。

## 图标约定

- 全部取自 IconMCP 的 **outline** 风格，线宽 1.5 px（按 24 px 计），圆角端点；
- 统一墨色 `#374151`（与 `light.qss` 的正文/工具按钮同色），不再混用品牌蓝/青/绿/红；
- 每个图标渲染 16/18/20/24/32/40/48/64 八档，`FwIcons::get()` 按真实尺寸取图，
  避免 Qt 从小图放大导致发虚；
- 应用图标是「品牌蓝渐变圆角底 + 白色包裹字形」(`appicon-glyph.svg` 由 build 脚本组合)，
  另出 128/256 与多尺寸 `.ico`，`FwIcons::appIcon()` 专供窗口/任务栏；
- 「缩略图显示」以前是运行期 `QPainter` 画的 2×2 色块，现在与树形/平铺一致走资源图标；
- 对话框的「确定/取消」不再加图标：默认按钮在主题里是实心蓝底，固定深色图标会糊在一起，
  按钮文字已足够清楚（合并/分割/导出三个对话框统一）。

## 注意

- **改了 `iconset.json` 里增删 slot 后要重新跑一次 qmake**：`resources.qrc` 的文件清单变了，
  Makefile 需要重新生成依赖；只换已有图标的字形/颜色则直接 `make` 即可。
- `appicon.ico` 被脚本更新时会顺带 touch `resources/appicon.rc`：qmake 的 windres 规则
  只看 `.rc` 的时间戳，不 touch 的话 exe 里嵌的还是旧图标。
- 本机 `svg2png.exe` 在受限沙箱里只能写工作目录，所以 `build-iconset.mjs` 先渲染到
  `tools/icons/work/render/` 再复制进 `resources/icons/`；直接渲染到 `resources/` 会
  报 `cannot save`（QImage 保存失败）。

## 目录

| 路径 | 作用 |
| --- | --- |
| `iconset.json` | 唯一清单：slot → IconMCP 名字、尺寸、墨色、应用图标参数 |
| `iconmcp.mjs` | 极简 Streamable-HTTP MCP 客户端（Node，支持 search/get/batch） |
| `build-iconset.mjs` | 拉取缺失字形 → 渲染 PNG → 组 appicon → 写 ICO/qrc |
| `svg2png/` | QtSvg 批量光栅化工具（源码与 .pro） |
| `fetch-candidates.mjs` | 从 IconMCP 批量下载候选字形到 `work/candidates/`，供人工挑 |
| `make_sheet.py` | 把候选/成品渲染图拼成对照表 |
| `shot-app.ps1` | 启动真实程序并截图（`-OpenFile` 载入示例归档、`-OpenMenu` 展开菜单） |
| `work/` | 本地草稿（候选、截图、示例归档），已 gitignore |

## 来源与许可

- 图标字形：IconMCP，MIT licence（https://iconmcp.com）。
- 本目录脚本：随项目源码分发；`work/` 里的图标预览图不是产品资源。
- IconMCP 没有 REST 下载接口，只有 MCP 端点 `https://iconmcp.com/mcp`。
  本机 PowerShell 的 schannel 取不到 TLS 凭证（`SEC_E_NO_CREDENTIALS`），
  所以 `iconmcp.mjs` 用 Node 的 OpenSSL 栈发请求；站点也接受 `http://` 端点。
