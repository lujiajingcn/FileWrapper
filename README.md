# FileWrapper

文件封装及查看工具（Qt 6 桌面应用）：把多个文件合并为 FWDA 归档，并按真实内容
自动路由到对应的查看器插件（Word / Sheet / Slides / PDF / 图片 / 文本 / 电子书 / 音视频）。
打开方式完全由程序判断（文件内容优先、插件声明的后缀兜底），不需要用户维护映射。

归档格式 v4 支持条目级「标记」：文件列表右键 → 标记…，标记随 `.fwda` 一起保存/分发，
左侧搜索框可直接按标记过滤。v1–v3 归档仍可读取，保存后升为 v4；旧版本程序会明确
拒绝打开 v4（不会静默丢掉标记）。

## 构建

前置：Qt 6.9.3 `mingw_64` + MinGW 13.1.0（本机路径见 `.workbuddy/memory/MEMORY.md`）。
使用影子构建，不要把产物写进源码树：

```powershell
$env:PATH = 'E:\V_C\Qt\Tools\mingw1310_64\bin;E:\V_C\Qt\6.9.3\mingw_64\bin;' + $env:PATH
mkdir _bw; cd _bw
qmake ..\FileWrapper.pro
mingw32-make -j6
mingw32-make deploydeps        # 补插件运行库（Qt6Xml/Qt6Pdf/FFmpeg/...）
```

`FileWrapper/deploy/plugin-deps.manifest` 是插件运行时依赖的唯一来源；新增依赖只改
manifest。修改插件 JSON 后，仅重跑 qmake 不会重嵌元数据，需删除该插件的
`moc_*.cpp`/对象或先 `mingw32-make clean`。

## 测试

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-plugin-tests.ps1
# 只跑逻辑用例（不生成截图）
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-plugin-tests.ps1 -SkipShots
```

测试把主程序源码直接编入，并用 `QPluginLoader` 加载真实构建出的插件 DLL；回归基线
与新增用例约定见 `tests/README.md`。

## 打包

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File FileWrapper\deploy\package.ps1 `
    -BuildDir _bw\FileWrapper\debug -OutDir ..\FileWrapper-package -Overwrite
```

`-OutDir` 必须是仓库外的目录（脚本拒绝仓库根及其任意子目录，避免递归删除源码）。
