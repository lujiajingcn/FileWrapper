# libmobi（vendored 源码）

本目录是 [libmobi](https://github.com/bfabiszewski/libmobi) 的**内嵌源码副本**，
由 FileWrapper 的 PluginBook 插件直接编译使用。

## 来源

| 项 | 值 |
|---|---|
| 上游仓库 | https://github.com/bfabiszewski/libmobi.git |
| 上游 commit | `906274205c11944b628da1c553b255acb1af7c55` |
| 上游日期 | 2024-10-29 |
| 许可证 | LGPL（见本目录 `COPYING`） |

上游为独立 Git 仓库，本项目以**普通目录**方式纳入版本控制（不是 submodule），
原因是 PluginBook 需要一个上游不存在的 `Makefile.mingw`（见下），
用 submodule 会导致克隆后缺少该文件而无法构建。

## 为什么单独用 Makefile.mingw 编译

`src/miniz.c` 采用 amalgamation 模式（`src/miniz.h` 内 `#include "miniz.c"`）：
其它 `.c` 通过 include `miniz.h` 只拿到声明，真正的 `mz_uncompress` 等定义
必须来自**单独编译的 `miniz.c`**。qmake 会把 `miniz.c` 误判为头文件依赖而丢弃，
导致链接期 `mz_uncompress` 未定义，因此这里独立编成 `libmobi.a` 再让插件链接。

`Makefile.mingw` 是本项目新增文件（上游没有），由 `PluginBook.pro` 的
`libmobi.target` 调用：

```pro
libmobi.target    = $$LIBMOBI_DIR/libmobi.a
libmobi.commands  = $(MAKE) -C $$LIBMOBI_DIR -f Makefile.mingw
```

其中加密相关的 `encryption.c` / `sha1.c` / `randombytes.c` **未编入**
（对应关闭 `USE_ENCRYPTION`），因此不需要链接 `advapi32`。
MOBI 的 DRM 加密本身也无法解密，不影响普通正文解析。

## 更新步骤

1. 在本目录执行 `git pull`（若上游仓库元数据仍在）；
   若 `.git` 已被清理，则重新克隆上游到临时目录，再覆盖本目录内容。
2. 保留 `Makefile.mingw`（本项目文件，不要被上游覆盖删除）。
3. 确认 `Makefile.mingw` 的 `SRCS` 列表与 `src/` 下的实际文件一致
   （上游新增/删除 `.c` 时需要同步）。
4. 重新构建：`mingw32-make -f Makefile.mingw clean && mingw32-make -f Makefile.mingw`。

## 构建产物

`libmobi.a` 与 `src/*.o` 是构建产物，已被仓库根目录的 `.gitignore` 忽略，
不纳入版本控制；克隆后由 `PluginBook.pro` 自动重新编译。
