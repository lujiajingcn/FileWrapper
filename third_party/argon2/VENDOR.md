# Argon2（vendored）

本目录是 **phc-winner-argon2** 参考实现的裁剪副本，供 FileWrapper 的
`PRODUCT_KEY` 派生使用。源码按原样保留，未做修改。

| 项 | 值 |
|---|---|
| 上游 | https://github.com/P-H-C/phc-winner-argon2 |
| 版本 | 20190702 发布包（`phc-winner-argon2-20190702.zip`） |
| 算法版本 | Argon2 v1.3（0x13） |
| 许可证 | CC0 1.0 / Apache-2.0（二选一），见 `LICENSE` |
| 引入日期 | 2026-11-20 |

保留的文件：

```
include/argon2.h
src/argon2.c src/core.c src/core.h
src/encoding.c src/encoding.h
src/ref.c
src/thread.c src/thread.h
src/blake2/
LICENSE README.md CHANGELOG.md
```

移除的内容：`kats/`、`latex/`、`man/`、`argon2-specs.pdf`、`src/{bench,run,test,genkat,opt}.*`
与各平台工程文件（`Argon2.sln`、`Makefile` 等）。`opt.c` 未启用：本项目只使用
`ref.c` 的参考实现，牺牲约 2–4 倍速度换取与官方 KAT 完全一致的确定性。

## 构建接入

- `FileWrapper/core.pri` 统一列出上述 `.c` 源码与 include 路径；
- 主程序与 `tests/plugin_tests.pro` 共用（二者都 include core.pri）；
- `tests/crypto_tests.pro` 单独列出同一批源码做单测；
- 不生成/链接 DLL，源码直接编入可执行文件。

## 校验

`tests/crypto_tests.cpp` 用官方 KAT 验证实现：

```
Argon2id v1.3, t=3, m=32 KiB, p=4
password=32×0x01, salt=16×0x02, secret=8×0x03, ad=12×0x04
期望 tag = 0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659
```

该用例由 `FwCrypto::argon2SelfTest()` 实现，必须保持通过；更换 Argon2 版本或
修改其源码时，必须重新跑通。

## 升级规则

**不要升级。** PRODUCT_KEY 的派生参数（Argon2 版本、m/t/p、盐）属于冻结范围，
升级实现可能改变输出，导致所有历史归档无法打开。确需升级时必须先完成
“全量归档迁移/重新加密”的专项评估。
