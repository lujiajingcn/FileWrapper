# FWDA v5 加密归档设计（用户配置密钥）

> 状态：**v1.2 已实现（参数冻结；主程序、单元测试、端到端测试、插件测试全部通过）**
> 日期：2026-11-20
> 基线代码：`FileWrapper/archiveformat.h`（当前 v4：明文 + CRC32 + 条目级标记）
> 本文只描述方案，不含代码改动。

## 决策记录（2026-11-20）

| 议题 | 结论 |
|---|---|
| 密钥分发模型 | **B：每台机器在设置里手动配置同一字符串**；程序不内置任何密钥 |
| 字符串为空 | 视为**未配置**，禁止合并；打开时引导去设置 |
| 历史密钥 | **保留**（最多 8 条，DPAPI 加密），打开旧归档时依次尝试 |
| 密钥派生 | Argon2id，参数与盐**永久冻结** |
| Argon2 参数 | **m=256 MiB、t=4、p=4，2026-11-20 确认冻结** |
| 密钥环上限 | **8 条，2026-11-20 确认** |
| 追溯/水印 | 不做 |
| 在线授权 | 不做 |

---

## 0. 结论摘要

1. 归档内容与目录全部加密：**AES-256-GCM**（Windows CNG）。
2. 每份归档随机生成 **CMK**（内容主密钥），用 **PRODUCT_KEY** 包裹后存文件头。
3. `PRODUCT_KEY = Argon2id(用户在设置里输入的字符串) → 32B`，**程序里不存任何密钥**。
4. 任意版本的 FileWrapper + **同一字符串** = 可打开；缺字符串、字符串不同 = 打不开。
5. 字符串为空视为未配置，合并被拦下并提示去设置。
6. 设置里保留历史字符串（密钥环），换钥后旧归档仍能打开。
7. **安全强度完全取决于字符串强度**：短串/常见短语可被离线穷举，程序只提示、不拦截。

---

## 1. 目标与非目标

### 1.1 目标

| 编号 | 目标 |
|---|---|
| G1 | 只拿到 `.fwda`（无程序、无密钥串）且密钥串足够强时，无法在可行时间内得到明文 |
| G2 | 打开不受版本限制：任意版本 FileWrapper + 同一密钥串即可打开任意 `.fwda` |
| G3 | 配置一次后零交互：不弹口令框、不联网、不绑设备 |
| G4 | 损坏、截断、篡改、块重排、跨文件拼接都能被检测并拒绝 |
| G5 | 合并/打开/导出/分割的操作习惯不变 |

### 1.2 非目标

- 不防合法用户的导出、截图、另存。
- 不做泄漏追溯（无水印、无台账）。
- 不防本机管理员 / 同 Windows 用户下的恶意程序（DPAPI 可解）。
- 不做在线授权、撤销、到期控制。
- 不保留 v1–v3 兼容分支（仓库当前策略：只有一种格式，且尚未发布）。

---

## 2. 安全边界与威胁模型

### 2.1 安全声明

> 在攻击者**只拥有 `.fwda`**（没有程序、不知道密钥字符串），且密钥字符串具有足够熵（建议 ≥ 80 bit）的前提下，解密不可行。

这是一条**带前提**的声明，前提由用户输入的字符串决定，不是程序能保证的。

### 2.2 攻击者矩阵

| 攻击者 | 能力 | 结果 |
|---|---|---|
| A1 外行 | 改后缀、7-Zip / 记事本 | 失败 |
| A2 半专业 | `strings`、hexdump、binwalk、foremost、自写解析器 | 失败：随机字节，无签名 |
| A3 有程序、无串 | 安装/拷贝 FileWrapper，未配置密钥 | **失败**（比“内置密钥”方案更强） |
| A4 有程序 + 弱串 | 猜/字典命中所配置的字符串 | **成功**：可由文件离线验证 |
| A5 同机同用户 | 解密 DPAPI 存储、读进程内存 | 成功——非目标 |
| A6 本机管理员 | dump、临时文件 | 成功——非目标 |

### 2.3 明确不能保证的

- 字符串是 `123456`、`admin`、`MyCompany2026` 这类内容时，任何 KDF 都救不了：`.fwda` 里的 `wrappedKey` 就是离线验证器，字典攻击一次命中。
- 字符串在多人之间共享时，任何一个人泄漏 = 所有用该串的归档失守；没有撤销手段。
- 字符串丢失 = 用该串加密的归档永久打不开。

---

## 3. 总体设计

### 3.1 密钥层级

```
用户字符串（设置界面输入，任意内容）
   │  Argon2id(固定盐, 固定参数) —— 确定性派生
   ▼
PRODUCT_KEY（32B，不落盘、不编译进程序，仅内存驻留）
   │  AES-256-GCM 包裹
   ▼
CMK（32B，每归档随机，存于文件头 wrappedKey）
   ├─ HKDF-SHA256(CMK, archiveUuid, "FWDA/v5/index")      → K_index  → 加密目录
   └─ HKDF-SHA256(CMK, fileNonce,  "FWDA/v5/file"+entryId) → K_file   → 加密该条目内容
```

### 3.2 密码学原语与依赖

| 用途 | 算法 | 实现 |
|---|---|---|
| 对称加密 / 认证 | AES-256-GCM，16B tag | Windows CNG（`bcrypt.dll`） |
| 密钥派生 | HKDF-SHA256 | CNG HMAC 或 Qt `QMessageAuthenticationCode` |
| 字符串 → 密钥 | Argon2id v1.3（0x13） | **vendored libargon2**（CC0/Apache-2.0） |
| 密钥串本地存储 | DPAPI | `CryptProtectData`（`crypt32.dll`） |
| 随机数 | CSPRNG | `BCryptGenRandom` |

> 为什么需要 libargon2：CNG 只有 PBKDF2，没有 Argon2。用户可输入任意短串，Argon2id 的内存硬化能显著抬高离线暴力成本，比 PBKDF2 更合适。vendored 源码编入主程序，不引入运行时 DLL。

### 3.3 与当前 v4 的关系

当前 v4 是明文格式（24B 全局头 + 明文目录 + 原样内容 + CRC32）。加密改变布局，因此版本号升为 **v5**：

- v5 读、v5 写；
- version != 5 的归档按现有策略明确拒绝；
- 仓库内测试产物重新生成即可，不需要迁移工具。

---

## 4. 密钥字符串与派生

### 4.1 形态

- 任意字符串：中文、空格、符号、大小写均允许，**不做内容校验、不做长度下限**。
- 空字符串 = 未配置（不参与派生）。
- 内部处理（不改变用户可见语义）：
  - 统一 `QString::normalized(NFC)` 后转 UTF-8；
  - **不 trim**、不做大小写折叠；若检测到首尾空白，仅提示“首尾含空格，请确认”，不拦截；
  - 输入长度上限 1024 字符（防误粘贴超大文本）。

### 4.2 派生参数（永久冻结）

| 参数 | 值 |
|---|---|
| 算法 | Argon2id，版本 0x13 |
| 盐 | ASCII `"FWDA/v5/product-key"`（固定常量，不随机） |
| 内存 m | 262144 KiB（256 MiB） |
| 迭代 t | 4 |
| 并行 p | 4 |
| 输出长度 | 32 字节 |
| 输入编码 | UTF-8（源字符串先 NFC 规范化） |

- 确定性：同字符串在任何机器、任何版本上派生出同一把钥。
- 盐不需要保密；固定盐是为了跨机器可复现。
- **这组参数一旦改动，旧归档全部打不开**——与格式一样属于冻结范围。

### 4.3 程序内不存密钥

- 二进制里没有 `PRODUCT_KEY`、没有密钥字符串、没有可用的派生捷径。
- 运行期只在需要时派生，内存中保存 32B 密钥，卸载归档/退出时 `SecureZeroMemory`。
- 不写日志、不进崩溃转储、不出现在命令行参数里。

### 4.4 未配置 / 不匹配

| 场景 | 行为 |
|---|---|
| 未配置（空串）时点「合并」 | 拦下，提示“请先在 设置 → 归档加密密钥 中配置密钥字符串”，并可直接打开设置 |
| 未配置时打开归档 | 同样引导去设置，不尝试解密 |
| 已配置但与归档不匹配 | 依次尝试密钥环；全部失败 → “没有匹配的密钥：请检查设置中的密钥字符串，或确认归档未损坏”；可选“临时输入其它字符串”试一次（不写入设置） |

---

## 5. 设置界面与密钥环

### 5.1 入口

- 新增顶级菜单 **「设置」**（现有为“文件处理 / 插件设置 / 帮助”）。
- 菜单项：「归档加密密钥…」。

### 5.2 界面

```
┌─ 设置 · 归档加密密钥 ──────────────────────────────┐
│ 当前密钥字符串                                      │
│ [ ••••••••••••••••••••••••••••••• ] [显示]         │
│ 强度：中        密钥指纹：9F3C-A1B2                 │
│ ⚠ 短字符串可被离线穷举；建议 ≥16 位随机字符。       │
│   字符串一旦丢失，用它加密的归档将无法打开。        │
│                                                     │
│ 历史密钥（最多 8 条，打开旧归档时依次尝试）         │
│ ┌─────────────────────────────────────┐           │
│ │ ● 9F3C-A1B2  （当前）        [删除] │           │
│ │ ○ 12AB-77CD                 [删除] │           │
│ └─────────────────────────────────────┘           │
│                                                     │
│ [应用]   [清除当前]   [清空全部]              [关闭] │
└─────────────────────────────────────────────────────┘
```

- 强度提示：按长度/字符集/常见词表估算，**只提示，不阻止**；常见短语直接标“弱”。
- 密钥指纹：`SHA-256(PRODUCT_KEY)` 前 4 字节，十六进制 8 位，用于多台机器核对“是否为同一把钥”。
- 「应用」：后台派生（进度光标/等待框，约 0.5–1.5 s）→ 计算指纹 → DPAPI 加密 → 写入设置 → 插入密钥环首位。
- 「清除当前」：移除当前项并让下一条生效；「清空全部」：二次确认后清空。

### 5.3 存储

沿用现有 QSettings（`IniFormat` + `UserScope`，即 `%APPDATA%\FileWrapper\FileWrapper.ini`）：

```ini
[Crypto]
KeyRing=9F3CA1B2|v1:<base64(DPAPI)>, 12AB77CD|v1:<base64(DPAPI)>, ...
```

- 列表项格式：`<指纹8位>|<版本前缀>|<base64(CryptProtectData(UTF-8 字符串))>`。
- DPAPI 使用 `CRYPTPROTECT_UI_FORBIDDEN`，并带固定附加熵 `"FileWrapper/v5/keyring/v1"`。
- 指纹随条目一起存，打开设置界面无需重新派生即可显示。
- 只有当前 Windows 用户能解出原文；换机器/换用户必须重新输入（模型 B 的预期行为）。

### 5.4 密钥环行为

- 打开归档时按“当前 → 历史”顺序尝试；成功一次即停止。
- 派生结果在会话内缓存（字符串哈希 → 32B），避免反复 Argon2；退出时清零。
- 最坏情况：8 条全部不匹配 ≈ 8 次派生（数秒），期间显示进度并允许取消。
- 新增字符串若与已有条目指纹相同：移到首位，不重复添加。
- 超过 8 条：丢弃最旧一条，并在应用时提示。

---

## 6. v5 文件格式

### 6.1 全局布局

```
┌──────────────────────────────┐ 0
│ 全局头 64B（明文，含索引 IV） │
├──────────────────────────────┤ 64
│ wrappedKey 60B               │
├──────────────────────────────┤ 124
│ 加密索引区 IndexCipher       │ 长度 = IndexCipherLen
├──────────────────────────────┤
│ 可选扩展区 Ext               │ 长度 = ExtLen（v5.0 = 0）
├──────────────────────────────┤ headerSize
│ 内容区（分块密文，顺序排列）  │
└──────────────────────────────┘ EOF
```

`headerSize = 64 + 60 + IndexCipherLen + ExtLen`；每条内容的 `contentPos` 是绝对偏移。

### 6.2 全局头（64 字节，小端）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | Magic | `"FWDA"` |
| 4 | 4 | Version | `5` |
| 8 | 4 | Flags | bit0 保留=0；bit1 压缩保留（**永远为 0**）；bit2 尾部填充；bit3–31 保留=0；未知位置位 → 拒绝 |
| 12 | 4 | KeyScheme | `1` = Argon2id 派生 + AES-256-GCM |
| 16 | 4 | IndexCipherLen | 加密索引区长度（含预留与 tag） |
| 20 | 4 | WrappedKeyLen | `60` |
| 24 | 4 | ExtLen | 扩展区长度，v5.0 为 0 |
| 28 | 4 | Reserved | 0 |
| 32 | 16 | ArchiveUuid | CSPRNG 随机，归档唯一 |
| 48 | 12 | IndexIv | 本次索引加密的 IV（每次重写头部换新） |
| 60 | 4 | Reserved2 | 0 |

> 全局头保持明文：解析器需要它定位索引、拿 IV。它不含文件名、大小、数量、时间等业务信息。

### 6.3 wrappedKey（60 字节）

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 12 | WkIv（随机） |
| 12 | 32 | AES-256-GCM(PRODUCT_KEY, CMK) 的密文 |
| 44 | 16 | Tag |

- `AAD_wk = magic(4) || version(4) || keyScheme(4) || archiveUuid(16)`，共 28 字节。
- AAD 只放文件生命周期内不变的字段，**不含** `flags` / `IndexIv` / `IndexCipherLen`：
  - 重写头部（换 IV、换长度、开关填充）不需要重新包裹 CMK；
  - 可变字段的完整性由索引 AEAD 保证（索引 AAD 覆盖完整 64B 全局头）。

### 6.4 加密索引区

明文索引结构（加密前）：

```
u32  rootLen
u8   root[rootLen]                     // UTF-8，现有语义不变
u32  entryCount
重复 entryCount 次：
  u32  recordLen                       // 本条剩余字节数（不含自身），便于跳过未知扩展
  u64  entryId                         // 归档内唯一随机数，条目身份
  u64  size                            // 明文长度
  u64  contentPos                      // 第一条密文块的绝对偏移
  u32  chunkCount                      // = ceil(size / 1 MiB)，0 字节文件为 0
  u8   fileNonce[8]                    // 该条内容的密钥/IV 派生参数
  u64  createdMs
  u64  modifiedMs
  u64  accessedMs
  u32  attributes
  u32  tagCount
  重复 tagCount 次：u32 tagLen / u8 tag[tagLen]
  u32  pathLen
  u8   path[pathLen]                   // 相对 root，现有安全校验不变
  …                                    // 未来字段追加在此，旧读取器用 recordLen 跳过
补零至索引容量（预留区）
```

- `K_index = HKDF-SHA256(CMK, salt=archiveUuid, info="FWDA/v5/index", L=32)`。
- `AES-256-GCM(K_index, IndexIv)`，AAD = **完整 64 字节全局头**。
- 沿用 `headerReserveFor()` 预留策略：`IndexCipherLen = 明文容量 + 16`；新增条目/改标记只要放得下就只重写头部。
- 解析时补零区必须被容忍。

> 条目带 `entryId` 而非数组下标：删除条目会让下标前移，若 AAD 绑定下标，剩余条目的内容将全部解不开。

### 6.5 内容分块

| 项 | 值 |
|---|---|
| 明文块大小 | 1 MiB（1048576） |
| 密文块长度 | 明文块长度 + 16（tag） |
| 块数 | `chunkCount = ceil(size / 1 MiB)` |
| 第 i 块偏移 | `contentPos + i * (1 MiB + 16)`（最后一块可能更短） |
| 文件密钥 | `K_file = HKDF-SHA256(CMK, salt=fileNonce, info="FWDA/v5/file" ‖ entryId_be64)` |
| 块 IV | `fileNonce(8B) ‖ chunkIndex_be32(4B)` |
| 块 AAD | `magic(4) ‖ version(4) ‖ archiveUuid(16) ‖ entryId_be64(8) ‖ chunkIndex_be32(4) ‖ chunkCount_be32(4)` = 40B |

绑定 `entryId + chunkIndex + chunkCount` 后：换块、删块、截断、跨文件搬运、跨归档拼接都会在解密时失败。

### 6.6 尾部填充（可选，默认关）

- `Flags.bit2 = 1` 时补 CSPRNG 随机字节，使文件大小为 64 KiB 整数倍。
- 读取器**必须**按“最后条目 contentPos + 密文长度”计算有效末端，**忽略其后所有字节**。
- 追加保存前先 `truncate` 到有效末端。

### 6.7 扩展区

- 重复 `{ u32 id; u32 len; u8 payload[len]; }`，`ExtLen` 为总长。
- 未知 id 必须跳过；v5.0 不定义“必需”扩展。

### 6.8 解析校验

| 校验 | 规则 |
|---|---|
| 版本 | 必须等于 5 |
| Flags | 未知位为 0；bit1 必须为 0 |
| KeyScheme | 必须等于 1 |
| WrappedKeyLen | 必须等于 60 |
| IndexCipherLen | ≥ 16 且 ≤ `MAX_HDR_SIZE` |
| headerSize | 不得溢出 qint64 |
| chunkCount | 与 size 一致；`size + chunkCount*16` 不溢出 |
| contentPos | ≥ headerSize；末端 ≤ 文件大小 |
| 路径/标记 | 沿用现有 `isSafeRelativePath` / `MAX_TAGS` / 归一化规则 |

AEAD 失败统一报“归档已损坏或被篡改”，不区分原因。

---

## 7. 加解密流程

### 7.1 合并（QMergeFiles）

1. 取当前 `PRODUCT_KEY`；未配置 → 拦截并引导设置。
2. 生成 `ArchiveUuid(16)`、`CMK(32)`；每条生成 `entryId(64)`、`fileNonce(8)`。
3. 生成明文索引并计算预留 → `IndexCipherLen`。
4. 写 64B 全局头占位；用 `PRODUCT_KEY` 包裹 CMK 写 `wrappedKey`。
5. 流式处理内容：读 1 MiB → 加密 → 写；记录 `contentPos/chunkCount`。
6. 用 `K_index` 加密索引，回填 `IndexIv` 等字段后写入。
7. 走现有“临时文件 → 备份 → 替换”原子流程。

### 7.2 打开（parseFromDevice）

1. 读 64B 全局头，按 §6.8 校验。
2. 取密钥环：当前密钥 + 历史密钥，逐个解 `wrappedKey`，成功即得 CMK。
3. 失败 → 提示“没有匹配的密钥”，可选临时输入其它字符串。
4. 解索引得 `root/entryCount/entries`，还原 `FILEINFO`。
5. 打开期间 CMK/PRODUCT_KEY 驻留内存受保护，卸载时清零。

### 7.3 只改头部保存（saveHeaderOnly）

- 重新生成 `IndexIv` → 重加密索引 → 写 header journal（旧前缀 + 旧索引密文）→ 写新前缀与索引 → fsync → 清理。
- `wrappedKey` 不动（AAD 不含 IndexIv）。
- 容量不足时退化为整包重写。

### 7.4 追加保存（saveAppendOnly）

- 先把新条目密文追加到有效内容末端（必要时先截掉尾部填充）。
- 重加密索引并写回。

### 7.5 整包重写（rewriteArchive / compactArchive）

- CMK 不变时可直接复制密文（同步复制 `entryId/fileNonce`），删除条目只跳过其密文。
- 需要换钥时逐条解密→重加密，并更新 `wrappedKey`。

### 7.6 读取内容

- `entryContent` / `extractEntryToTempFile` / `outputFile` / `QSplitFiles`：按块解密后写出。
- `verifyArchive`：逐条解密校验 tag，统计损坏条目。
- 查看器：
  - ≤ 256 MiB：解密到内存，`sendFileMapped` 可用（映射解密后的缓冲）；
  - > 256 MiB：流式解密到临时文件（随机名、`FILE_ATTRIBUTE_TEMPORARY`、关闭即删）；
  - `MainWindow` 里对归档文件的 `QFile::map` 必须移除（映射到的是密文）。

---

## 8. 冻结规则

G2（任意版本互开）要求**密钥派生与解码规则都不变**。

1. **布局冻结**：§6 的字段、顺序、语义从 v5.0 起不再修改。
2. **派生参数冻结**：§4.2 的盐、m/t/p、Argon2 版本、输入编码不得改动；否则旧归档全部打不开。
3. **只增不改**：新字段只能追加在 `recordLen` 覆盖范围内，或放进 Ext 区。
4. **压缩永久不启用**：`Flags.bit1` 保留且必须为 0（解压需要新代码，旧版本打不开）。
5. **填充可后启**：读取器从 v5.0 起就忽略尾部多余字节。
6. 版本号只在破坏性变更时递增；递增即失去 G2。

---

## 9. 代码改造清单

| 文件 | 位置（当前） | 改动 |
|---|---|---|
| `archiveformat.h` | 全局定义 | 新增 v5 常量与结构：`VERSION_V5=5`、`GLOBAL_HDR_V5=64`、`WRAPPED_KEY_LEN=60`、`CHUNK_SIZE`；`Entry` 增加 `entryId/fileNonce/chunkCount` |
| `filemanager.h` | `FILEINFO` | 增加 `entryId/fileNonce/chunkCount`（或由 Meta 侧映射） |
| `filemanager.cpp` | `parseFromDevice:446` | v5 解析：前缀校验 → 密钥环解包 CMK → 解索引 |
| | `buildHeaderBuffer:1400`、`buildHeaderOnlyBuffer:1475`、`writeArchiveHeader:2016` | 生成明文索引后加密；写 64B 前缀 |
| | `finalizeArchiveHeader:2144` | 改为写索引 AEAD tag，删除 CRC 回填 |
| | `streamCopy:2191` | 分块加密写入，返回密文长度 |
| | `QMergeFiles:2243` | 取 PRODUCT_KEY；未配置则拦截；生成 uuid/CMK/entryId/fileNonce |
| | `saveHeaderOnly:1602`、`saveAppendOnly:1762` | 重加密索引；密文追加 |
| | `rewriteArchive:2816`、`compactArchive:1985` | 密文复制或重加密 |
| | `openEntrySource:690`、`entryContent:3439`、`extractEntryToTempFile:970` | 解密读取 |
| | `outputFile:3520`、`QSplitFiles` | 解密导出 |
| | `verifyArchive:3641` | 改为 AEAD 校验 |
| `mainwindow.cpp` | `map@2185`、`entryContent@2340`、`extractToTempFile@2066` | 去掉对归档的 `QFile::map`；改用解密缓冲/临时文件 |
| | 菜单初始化（`mainwindow.ui` 三级菜单） | 新增「设置」菜单与「归档加密密钥…」动作；合并入口拦截未配置 |
| `core.pri` | 源文件清单 | 加入 `crypto/*`、`dlgsettings.*`、`third_party/argon2/*` |
| `FileWrapper.pro` | win32 | `LIBS += -lbcrypt -lcrypt32` |
| `.gitignore` | — | 无新增需要（不再生成密钥文件） |

新增模块：

```
FileWrapper/crypto/cngcipher.h/.cpp      随机数、AES-256-GCM、HKDF-SHA256
FileWrapper/crypto/argonderive.h/.cpp    Argon2id 封装 + 测试向量校验
FileWrapper/crypto/keyring.h/.cpp        DPAPI 存取、派生缓存、指纹、历史环
FileWrapper/crypto/archivecrypto.h/.cpp  v5 包裹/索引/分块加解密（纯函数）
FileWrapper/dlgsettings.h/.cpp/.ui       设置对话框
third_party/argon2/                      libargon2 源码 + LICENSE
```

---

## 10. 构建与发布

- **不再需要密钥注入**：程序不含任何秘密，构建完全可复现。
- `third_party/argon2` 以**固定版本**纳入仓库（含 LICENSE），构建不依赖网络。
- 金标准归档使用**公开测试串**（如 `FWDA-GOLDEN-v5`）生成，测试无需真实密钥。
- 发布检查单：
  - `strings FileWrapper.exe` 搜不到任何密钥字符串；
  - 金标准归档可开；
  - 归档经 binwalk / 7-Zip 检查无内嵌签名；
  - 签名 + 哈希归档。

---

## 11. 程序加固

由于密钥不在二进制里，逆向程序**拿不到 KEY**，只能拿到格式与派生参数。加固的重点从“保护密钥”降级为“保护完整性与体验”：

| 措施 | 说明 | 优先级 |
|---|---|---|
| 去符号 + LTO + `-O2` | 发布构建 `-s` | 建议 |
| 代码签名 | 防篡改，提高门槛 | 建议 |
| 关闭崩溃转储 | 避免内存中的密钥/CMK 落盘 | 必须 |
| 反调试/商业壳 | 收益明显下降（无钥可抠），不建议投入 | 可选 |

---

## 12. 兼容性与金标准

1. `tests/fixtures/golden_v5.fwda`：用公开测试串生成，含多条目、0 字节、跨块文件、中文路径、满标记。
2. CI 每次构建：用测试串打开金标准并逐字节比对导出内容。
3. Argon2 测试向量：`Argon2id("FWDA-TEST-VECTOR", salt, 参数)` 的期望值写进单测，防止参数漂移。
4. 保留首发版 exe，定期验证“旧版打开新归档”。

---

## 13. 测试矩阵

| 类别 | 用例 |
|---|---|
| 格式往返 | 0B / 1B / 1 MiB±1 / 4 GiB+；中文路径；32 标记；空根 |
| 加密正确性 | 合并→打开→导出逐字节一致；同内容两次合并密文不同 |
| 篡改检测 | 改前缀、wrappedKey、tag、密文块；删块、换块、换文件、换 uuid |
| 密钥 | 未配置拦截合并；错误串提示；历史环命中；临时输入；空串 |
| 设置 | DPAPI 往返；指纹一致；重复串去重；超过 8 条淘汰；清除当前/清空 |
| 保存路径 | 只改头部、追加、删除、整包重写、compact 后仍可打开 |
| 崩溃恢复 | 头部 journal 恢复；写内容中途取消 |
| 大文件 | >256 MiB 临时解密；退出后临时文件不存在 |
| 兼容 | 金标准双向；旧版 exe 打开新归档 |
| 性能 | 1 GB 合并 ≥300 MB/s；打开固定开销 <50 ms（密钥已缓存）；冷启动派生 0.5–1.5 s |
| 安全扫描 | strings / binwalk / 7-Zip 无法得到内容或文件名 |

---

## 14. 实施计划

| 阶段 | 内容 | 估时 |
|---|---|---|
| P1 | CNG 加密层 + libargon2 接入 + v5 写/读 + 合并/打开主链路 + 最小设置对话框（输入串即可用） | 6–8 天 |
| P2 | 只改头部/追加/整包重写/导出/分割/校验 + 大文件临时解密 + 崩溃恢复 | 4–5 天 |
| P3 | 完整设置界面（密钥环、指纹、强度提示、DPAPI）+ 未配置拦截 + 临时密钥入口 | 3–4 天 |
| P4 | 加固/金标准/全量测试/文档 | 2–3 天 |

合计约 15–20 个工作日。P1 结束即可内部试用；对外发布以 P4 验收为准。

---

## 15. 残余风险

| 风险 | 说明 | 缓解 |
|---|---|---|
| 弱字符串 | 文件是最强验证器，字典/暴力可直接命中 | UI 提示 + 建议；无法强制 |
| 字符串丢失 | 该串加密的归档永久打不开 | 用户自行备份；密钥环只能防“换钥”，防不了“丢钥” |
| 字符串泄漏 | 所有用该串的归档失守，无法撤销 | 换串后旧归档仍可用历史串打开，但历史串必须删除 |
| 多机配置负担 | 每台机器手动输入 | 模型 B 的固有代价 |
| 同用户恶意程序 | DPAPI 可解、内存可读 | 非目标 |
| 历史环膨胀 | 最多 8 条，超出淘汰最旧 | 应用时提示 |
| 性能 | 冷启动 Argon2 约 0.5–1.5 s | 会话内缓存；进度提示 |
| 格式冻结 | 未来无法加入需新解码逻辑的功能 | 功能集已定死：不压缩、不换块大小 |

**对外表述**：写“归档内容经 AES-256 加密，需在 FileWrapper 中配置密钥后打开”，**不要**写“无法破解”。

---

## 16. 参数冻结记录

以下参数已于 **2026-11-20 确认并冻结**，后续不得修改（修改 = 旧归档全部打不开）：

| 参数 | 冻结值 |
|---|---|
| KDF | Argon2id v0x13 |
| 盐 | `"FWDA/v5/product-key"` |
| 内存 m | 262144 KiB（256 MiB） |
| 迭代 t | 4 |
| 并行 p | 4 |
| 输出 | 32 字节 |
| 密钥环上限 | 8 条 |

不影响兼容的可选项：

| # | 事项 | 说明 |
|---|---|---|
| O1 | 界面显示密钥指纹 | 已设计，用于多机核对 |
| O2 | 历史环超过 8 条时淘汰最旧 | 应用时提示即可 |

---

## 附录 A：常量

| 常量 | 值 |
|---|---|
| `VERSION_V5` | 5 |
| `GLOBAL_HDR_V5` | 64 |
| `WRAPPED_KEY_LEN` | 60 |
| `TAG_LEN` | 16 |
| `CHUNK_SIZE` | 1 MiB |
| `PRODUCT_KEY_BYTES` | 32 |
| `ARGON2_SALT` | `"FWDA/v5/product-key"` |
| `ARGON2_M_KIB` | 262144 |
| `ARGON2_T` | 4 |
| `ARGON2_P` | 4 |
| `KEYRING_MAX` | 8 |

## 附录 B：设置存储示例

```ini
[Crypto]
KeyRing=9F3CA1B2|v1:AQAAANCMnd8BFdERjHoAwE/Cl+sBAAAA...,12AB77CD|v1:AQAAANCMnd8BFdERjHoAwE/Cl+sBAAAA...
```

- 每项 = `指纹8位 | v1:base64(DPAPI 密文)`。
- 第一项为当前生效密钥。

## 附录 C：测试向量（公开，非真实密钥）

```
输入:  "FWDA-TEST-VECTOR"
盐:    "FWDA/v5/product-key"
参数:  Argon2id v=0x13, m=262144, t=4, p=4, out=32
期望:  首次实现时生成，写入单测（后续不得变化）
```

## 附录 D：v4 → v5 字段对照

| 信息 | v4（现状） | v5 |
|---|---|---|
| 全局头 | 24B，含 FileCount、CRC32 | 64B，不含数量/CRC，含 uuid/IV |
| 文件数 | 明文 | 加密索引内 |
| 路径/标记/时间/属性 | 明文 | 加密索引内 |
| 内容 | 原样 | 分块 AES-256-GCM |
| 完整性 | CRC32 | GCM tag |
| 密钥 | 无 | 用户字符串 → Argon2id → PRODUCT_KEY → CMK |
| 版本兼容 | 仅 v4 | 仅 v5 |

---

## 附录 E：实施进度（2026-11-20）

| 模块 | 状态 |
|---|---|
| `third_party/argon2`（20190702，KAT 校验） | ✅ |
| `crypto/cngcipher`（AES-GCM / HKDF / CSPRNG） | ✅ |
| `crypto/argonderive`（Argon2id 派生 + 自检 + 指纹） | ✅ |
| `crypto/archivecrypto`（v5 头/wrappedKey/索引/分块） | ✅ |
| `crypto/archiveindex`（索引序列化/解析） | ✅ |
| `crypto/keyring`（DPAPI + 密钥环 + 解包） | ✅ |
| `FileManager` v5 解析（解密索引 + 内容区校验） | ✅ |
| 合并写入（占位头 → 加密内容 → 回填索引） | ✅ |
| 整包重写 / 整理归档 | ✅ |
| 只改头部保存 / 追加保存 | ✅ |
| 读取链路（entryContent / 导出 / 分割 / 校验） | ✅ |
| 查看器（禁用归档 mmap，改解密缓冲/临时文件） | ✅ |
| 设置界面（输入串 + 密钥环 + 指纹） | ✅ |
| 单元测试 `tests/crypto_tests` | ✅ 36 项 |
| 端到端测试 `tests/fwda_tests` | ✅ 42 项（含密钥环 DPAPI、设置对话框冒烟） |
| `tests/plugin_tests` | ✅ 301 PASS / 0 FAIL / 1 SKIP（受限沙箱跳过 D6 子进程用例） |
| 主程序构建 | ✅ `_bw/appcheck` 链接通过 |
