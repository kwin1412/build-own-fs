# build-own-fs

一个用于学习文件系统原理的极简 C17 文件系统原型。应用通过回调提供整块设备
读写；文件系统在内存中维护超级块、位图、inode 表和目录表，并在
`lfs_file_close` / `lfs_unmount` 时统一写回元数据。

> **状态：v1 教学原型。** 磁盘格式版本为 `LFS_VERSION = 1`。每个文件只使用
> 一个数据块（逻辑长度上限为 `block_size`），接口和磁盘格式仍可能变化，
> 请勿用于保存真实数据。

## 目录

- [快速开始](#快速开始)
- [功能与边界速览](#功能与边界速览)
- [设计概览](#设计概览)
- [仓库结构](#仓库结构)
- [构建与测试](#构建与测试)
- [运行示例](#运行示例)
- [磁盘格式](#磁盘格式)
- [接口参考](#接口参考)
- [常见问题](#常见问题)
- [已知问题与限制](#已知问题与限制)
- [后续方向](#后续方向)
- [代码风格](#代码风格)

## 快速开始

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

# 运行示例：示例不会创建镜像，需要先准备 4 MiB 的 fs.img
mkdir -p /tmp/lfs-demo && cd /tmp/lfs-demo
truncate -s 4M fs.img
/path/to/build/build_own_fs
```

ASan/UBSan、测试用例说明和示例输出分别见[构建与测试](#构建与测试)与
[运行示例](#运行示例)。

## 功能与边界速览

| 项目 | 当前值或行为 |
| --- | --- |
| 磁盘格式 | `LFS_VERSION = 1` |
| 块大小 | 最小 1024 B，最大 `INT32_MAX` |
| 示例设备 | 4096 B × 1024 块 = 4 MiB |
| 单文件最大长度 | `block_size`（示例为 4096 B） |
| 同时文件数上限 | 受数据块、目录项和 inode 数共同限制（示例为 999） |
| 文件名 | 1–64 字节，按字节精确比较，不解析路径 |
| 目录结构 | 仅根目录，扁平命名空间 |
| 打开语义 | `lfs_file_open` 兼作创建；不截断已有文件 |
| 元数据同步点 | `lfs_file_close`、`lfs_unmount` |
| 崩溃恢复 | 无日志、无恢复、无 fsck |
| 并发与锁 | 无 |
| 镜像可移植性 | 仅相同 ABI（端序/对齐/填充一致）可读 |

## 设计概览

```text
┌──────────────────────────────────────────────┐
│ 应用                                         │
│   lfs_config_t：块大小、块数量、read/write   │
└───────────────────┬──────────────────────────┘
                    │ lfs_* API
┌───────────────────▼──────────────────────────┐
│ lfs.c                                        │
│   内存：超级块、块位图、inode 位图、         │
│         inode 表、目录表                     │
└───────────────────┬──────────────────────────┘
                    │ 整块 read/write 回调
┌───────────────────▼──────────────────────────┐
│ 块设备（由应用提供，核心库不直接操作文件）   │
└──────────────────────────────────────────────┘
```

几个关键取舍：

- **元数据常驻内存。** 挂载时把位图、inode 表和目录表整体读入内存，后续操作
  只改内存；关闭文件或卸载时再整体写回。
- **磁盘布局简单。** 超级块、位图、inode 表和目录表依次排列，剩余块作为数据
  区；没有额外的数据块空闲链表，块分配状态由位图重建。
- **单块文件。** 每个文件只分配一个数据块，inode 只记录该块号，读取和写入都
  不需要块映射。
- **目录项即链表节点。** 目录表项通过 `next_entry_id` 组成有效链和空闲链，
  创建时从空闲链摘取，删除时挂回空闲链。
- **同步是显式的。** 没有后台回写或日志；`close`/`unmount` 是仅有的元数据
  同步点。

生命周期：

```text
未挂载 ── lfs_format ──► 未挂载 ── lfs_mount ──► 已挂载
   ▲                                               │
   └───────────── lfs_unmount（先同步） ◄──────────┘
```

`lfs_format` 成功后实例仍未挂载，需要再调用 `lfs_mount`；两者都不接受已经
持有内存表的实例。

## 仓库结构

```text
.
├── CMakeLists.txt
├── AGENTS.md              # 工程约定
├── include/
│   ├── lfs.h              # 公开接口：配置、句柄、文件 API
│   └── lfs_struct.h       # 磁盘结构：超级块、inode、目录项
├── src/
│   ├── lfs.c              # 文件系统全部实现
│   └── main.c             # 基于 POSIX 文件镜像的计数器示例
├── tests/                 # CTest 测试与内存块设备
│   ├── CMakeLists.txt
│   ├── test_support.[ch]  # 内存设备、CRC 与故障注入
│   ├── test_format_mount.c
│   ├── test_file_io.c
│   ├── test_dir_ops.c
│   └── test_metadata.c
└── fs.img                 # 示例镜像（运行示例可能覆盖或扩展）
```

`CMakeLists.txt` 显式列出库源码，静态库 `lfs` 只包含 `src/lfs.c`；示例程序
和测试分别链接该库，不会把 `main.c` 编入库中。

## 构建与测试

依赖：CMake ≥ 3.16、支持 C17 的 C 编译器。无第三方库。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

产物：

- `build/liblfs.a`：静态库（只包含 `src/lfs.c`）
- `build/build_own_fs`：示例程序
- `build/tests/test_*`：测试可执行文件

测试链接 `lfs` 库，不直接编译实现文件；测试使用内存块设备，不会读写
`fs.img`。

### ASan/UBSan

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON -DLFS_SANITIZE=ON
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    ctest --test-dir build-asan --output-on-failure
```

`LFS_SANITIZE` 仅支持 GCC 或 Clang。

### 测试用例

CTest 注册了 4 个测试程序：

| CTest 名称 | 源文件 | 主要覆盖内容 |
| --- | --- | --- |
| `lfs.format_mount` | `tests/test_format_mount.c` | 格式化/挂载往返、超级块与布局、魔数/版本/CRC/布局损坏、配置校验、读写回调故障注入 |
| `lfs.file_io` | `tests/test_file_io.c` | 空文件、读写与持久化、读改写、单块容量与短写、seek 边界、回调失败时 offset/length 不变、非法句柄、未 close 直接 unmount |
| `lfs.dir_ops` | `tests/test_dir_ops.c` | 重复打开、删除与空闲链复用、空闲计数、损坏元数据、重命名规则与 64 字节名字、断链、元数据位图保护 |
| `lfs.metadata` | `tests/test_metadata.c` | 跨多块的 inode 表/目录表（140 个文件、512 块设备）、1024/4096 字节块布局、变更后的 CRC、dump 环检测 |

测试设备默认使用 1024 B 块：128 块布局的数据区起点为块 14，512 块布局为
块 47；另有一组 4096 B × 1024 块的布局用例。运行单个测试：

```sh
ctest --test-dir build -R lfs.file_io --output-on-failure
```

## 运行示例

`src/main.c` 实现了一个 POSIX 镜像设备，配置为 4096 B × 1024 块（4 MiB），
并在镜像中维护 `boot_count` 计数器：

1. 先挂载镜像；仅当挂载失败（例如空镜像）时格式化；
2. 打开或创建 `boot_count`，读取旧值；
3. 值加一后重定位并写回，关闭文件；
4. 打印超级块和目录，然后卸载；再次运行可看到计数递增。

设备回调以 `"r+b"` 打开当前目录下的 `./fs.img`，**不会创建文件**，因此运行
前需要预置一个至少 `block_size × block_count = 4096 × 1024 = 4 MiB` 的镜像。
建议在临时目录中运行，避免覆盖仓库内的镜像：

```sh
mkdir -p /tmp/lfs-demo
cd /tmp/lfs-demo
truncate -s 4M fs.img
/path/to/build/build_own_fs
```

典型输出（CRC 和部分字段可能因版本或 ABI 不同而变化）：

```text
stored boot_count: 0
super block:
  magic: 0x004c4653
  version: 1
  block_size: 4096
  total_block_count: 1024
  free_block_count: 998
  total_inode_count: 1024
  free_inode_count: 1023
  block_bmp_block_id: 1
  inode_bmp_block_id: 2
  inode_table_block_id: 3
  dir_table_block_id: 5
  valid_dir_entry_id: 0
  free_dir_entry_id: 1
  crc32: 0x6ca452fb
directory:
  [0] name="boot_count" inode_id=0 size=4 block_id=25 next_inode_id=4294967295
boot_count: 1
```

再次运行同一镜像会输出 `stored boot_count: 1`，最后打印 `boot_count: 2`，
其余字段基本不变。示例的数据区从块 25 开始，初始有 999 个空闲数据块；
`boot_count` 占用块 25 后，空闲块计数为 `999 - 1 = 998`，空闲 inode 计数为
`1024 - 1 = 1023`。这两项与目录项中的 `size=4`、`block_id=25` 一起表明长度、
分配信息和目录内容已经持久化。

> **注意：** 示例在挂载失败时会格式化并覆盖镜像。不要用包含重要数据的镜像
> 运行它；如果 `fs.img` 不存在，挂载和格式化都会失败。仓库中的 `fs.img` 只是
> 示例文件，可能与当前 `BLOCK_SIZE` / `BLOCK_COUNT` 不一致；首次运行会重新
> 格式化，必要时把文件扩展到所需大小。修改这两个宏后，需要同步调整镜像大小
> 并重新构建。

## 磁盘格式

### 整体布局

块 0 固定为超级块，随后依次是块位图、inode 位图、inode 表和目录表，剩余块
构成数据区：

```text
+-------------+--------------+--------------+-------------+-------------+------------+
| super block | block bitmap | inode bitmap | inode table | dir table   | data blocks|
+-------------+--------------+--------------+-------------+-------------+------------+
      块 0          连续块         连续块         连续块        连续块        剩余块
```

示例配置（4096 B × 1024 块，4 MiB）下的实际分布：

| 块号 | 用途 | 占用 |
| --- | --- | --- |
| 0 | 超级块 | 1 块 |
| 1 | 块位图 | 1 块 |
| 2 | inode 位图 | 1 块 |
| 3–4 | inode 表（1024 × 8 B = 8192 B） | 2 块 |
| 5–24 | 目录表（1024 × 80 B = 81920 B） | 20 块 |
| 25–1023 | 数据区 | 999 块 |

各区域块数由以下公式计算（`DIV_ROUND_UP` 为向上取整）：

```text
块位图块数     = ceil(total_block_count / (block_size × 8))
inode 位图块数 = ceil(total_inode_count / (block_size × 8))
inode 表块数   = ceil(total_inode_count × sizeof(inode_t) / block_size)
目录表块数     = ceil(total_block_count × sizeof(dir_entry_t) / block_size)

data_start = 1 + 块位图块数 + inode 位图块数
               + inode 表块数 + 目录表块数
```

`lfs_format` 把 inode 总数设为块总数（`total_inode_count = block_count`），
因此 inode 表和目录表的容量都随块数量增长。布局必须至少留下一个数据块，
否则格式化和挂载都会失败。格式化时把 `[0, data_start)` 的元数据块在块位图
中标记为已占用，把所有数据块写成零块，然后逐块写回元数据并同步超级块。

**容量示例（4096 B × 1024 块）：**

- 元数据占用块 0–24，共 25 块；数据区为块 25–1023，共 999 块。
- 每个文件占用 1 个数据块、1 个目录项和 1 个 inode，因此最多同时存在
  `min(999, 1024, 1024) = 999` 个文件。
- 单文件有效载荷上限为 4096 B；即使只写入 1 B，也会占用整个块。
- 全部文件的有效载荷理论上限为 `999 × 4096 = 4,091,904 B`（约 3996 KiB）。

### 超级块（`super_block_t`，56 B）

以下偏移基于常见的 4 字节对齐 ABI；字段本身均为宿主端序的 `uint32_t`。

| 偏移 | 字段 | 含义 |
| --- | --- | --- |
| 0 | `magic` | `0x004C4653`（`"LFS"`） |
| 4 | `version` | 格式版本，当前为 1 |
| 8 | `block_size` | 块大小，有效范围 `1024 .. INT32_MAX` |
| 12 | `total_block_count` | 设备总块数，必须小于 `0xFFFFFFFF` |
| 16 | `free_block_count` | 空闲数据块计数（不含元数据区） |
| 20 | `total_inode_count` | inode 总数，格式化时取块总数 |
| 24 | `free_inode_count` | 空闲 inode 计数 |
| 28 | `block_bmp_block_id` | 块位图起始块 |
| 32 | `inode_bmp_block_id` | inode 位图起始块 |
| 36 | `inode_table_block_id` | inode 表起始块 |
| 40 | `dir_table_block_id` | 目录表起始块 |
| 44 | `valid_dir_entry_id` | 有效目录项链表头，`0xFFFFFFFF` 表示空 |
| 48 | `free_dir_entry_id` | 空闲目录项链表头，`0xFFFFFFFF` 表示空 |
| 52 | `crc32` | 对偏移 0–51 共 52 字节计算的 CRC-32，每次 sync 重算 |

CRC 使用初值和结果异或均为 `0xFFFFFFFF`、反射多项式 `0xEDB88320` 的标准
CRC-32（与 zlib 的 `crc32` 一致），只覆盖超级块，不覆盖位图、inode 表和
目录表。

### inode（`inode_t`，8 B）

| 偏移 | 字段 | 含义 |
| --- | --- | --- |
| 0 | `block_id` | 文件唯一数据块的块号 |
| 4 | `next_inode_id` | 预留的 inode 链指针，当前恒为 `0xFFFFFFFF` |

### 目录项（`dir_entry_t`，80 B）

| 偏移 | 字段 | 含义 |
| --- | --- | --- |
| 0 | `inode_id` | 指向 inode 表的下标 |
| 4 | `length` | 文件逻辑长度（字节），上限为 `block_size` |
| 8 | `next_entry_id` | 链表中的下一个目录项，`0xFFFFFFFF` 表示链尾 |
| 12 | `name[65]` | 文件名，最长 64 字节，含结尾 `\0` |
| 77–79 | 填充 | 对齐到 4 字节边界，内容无意义 |

目录表项数等于 `total_block_count`，所有项组成两条单链表：

- **有效链**：从 `valid_dir_entry_id` 开始，查找时顺序遍历；
- **空闲链**：从 `free_dir_entry_id` 开始，格式化时把所有项串成
  `0 → 1 → … → 总块数-1 → INVALID`。

创建文件时从空闲链取头节点并插入有效链头部；删除文件时先从有效链摘除、
清空字段，再把节点挂回空闲链头部。名字按字节精确比较，不做路径解析，
`/` 没有特殊含义。

### 位图与分配

`bmp_t` 为 `uint8_t`，每个 bit 表示一个块或一个 inode，`1` 表示已占用，
`0` 表示空闲；位序为字节内从低位到高位。分配采用首次适配：

- 数据块从 `data_start` 开始扫描块位图，找到第一个为 0 的位；
- inode 从 0 开始扫描 inode 位图；
- 分配/释放同步维护超级块中的空闲计数；
- 挂载时从位图重新计算空闲计数，而不是直接信任超级块中的旧值。

### 一致性范围

- 挂载校验超级块 CRC、魔数、版本、配置匹配和元数据区布局，并重算空闲计数；
  但**不**校验每条目录项、inode 和位图之间的完整一致性。
- `lfs_dump` 会检查有效目录链是否越界或成环，并检查目录项长度、inode 编号和
  数据块范围；它只读内存，不修复设备。
- 同步顺序为块位图、inode 位图、inode 表、目录表，最后写超级块；没有日志，
  中途失败或掉电仍可能留下不一致镜像。

### 端序与 ABI

- 磁盘结构直接按宿主机的结构体布局写入设备，没有显式的小端编码或字段
  序列化。
- 所有整数使用宿主端序；`dir_entry_t` 的填充字节也不参与解释。
- 只有相同 ABI（端序、对齐、填充一致）的平台上才能读取同一镜像。
- 结构体大小在编译期只通过 `static_assert` 约束为小于最小块大小；格式演进
  时应改为显式小端编解码，检查溢出和边界，并提升 `LFS_VERSION`。

## 接口参考

### 配置与句柄

```c
typedef int block_dev_read(void *buffer, uint32_t block_id);
typedef int block_dev_write(const void *buffer, uint32_t block_id);

typedef struct {
    block_dev_read *read;
    block_dev_write *write;
    uint32_t block_size;   // 格式化/挂载时使用
    uint32_t block_count;  // 格式化/挂载时使用
} lfs_config_t;

typedef struct {
    block_dev_read *read;
    block_dev_write *write;
    super_block_t sb;
    bmp_t *block_bmp;
    bmp_t *inode_bmp;
    inode_t *inode_table;
    dir_entry_t *dir_table;
} lfs_t;

typedef struct {
    uint32_t block_id;      // 文件唯一数据块
    uint32_t length;        // 文件逻辑长度，0..block_size
    uint32_t offset;        // 当前读写位置，0..length
    uint32_t dir_entry_id;  // 句柄关联的目录项，用于回写长度
} lfs_file_t;
```

### 设备回调约定

- 每次传输一个完整块，`buffer` 至少为 `block_size` 字节。
- 成功返回 0，任何非零值表示失败。
- 写回调负责把整块持久化；读回调必须填满整块。
- 回调没有上下文参数，应用通常通过全局变量或静态状态绑定具体设备。

### API 原型

```c
int lfs_mount(lfs_t *fs, const lfs_config_t *config);
int lfs_unmount(lfs_t *fs);
int lfs_format(lfs_t *fs, const lfs_config_t *config);
int lfs_dump(lfs_t *fs);

int lfs_file_open(lfs_t *fs, lfs_file_t *file, const char *path);
int lfs_file_close(lfs_t *fs, lfs_file_t *file);
int lfs_file_read(lfs_t *fs, lfs_file_t *file, void *buffer, uint32_t size);
int lfs_file_write(lfs_t *fs, lfs_file_t *file, const void *buffer,
                   uint32_t size);
int lfs_file_rewind(lfs_t *fs, lfs_file_t *file);
int lfs_file_seek(lfs_t *fs, lfs_file_t *file, int32_t offset,
                  uint32_t whence);
int lfs_file_dump(lfs_t *fs, lfs_file_t *file);
int lfs_file_remove(lfs_t *fs, const char *path);
int lfs_file_rename(lfs_t *fs, const char *oldpath, const char *newpath);
```

`lfs_mount` 分配四张内存表，`lfs_unmount` 释放它们。调用方必须把 `lfs_t`
和 `lfs_file_t` 零初始化；重复 `lfs_mount` 或对已挂载实例调用 `lfs_format`
都会返回 -1。

### 文件系统 API

| 函数 | 说明 | 返回值 |
| --- | --- | --- |
| `lfs_format(fs, cfg)` | 校验配置和布局，初始化超级块、位图和两张表，清零数据区并同步；成功后实例仍未挂载，需要再调用 `lfs_mount` | 0 或 -1 |
| `lfs_mount(fs, cfg)` | 读入超级块，校验魔数、版本、CRC、配置和布局，逐块读入元数据并从位图重算空闲计数 | 0 或 -1 |
| `lfs_unmount(fs)` | 已挂载时同步元数据，然后释放内存表；未挂载或同步失败返回 -1，但内存总会释放 | 0 或 -1 |
| `lfs_dump(fs)` | 打印超级块和有效目录链，检查越界/成环；只读内存，不写设备 | 0 或 -1 |

### 文件 API

| 函数 | 说明 |
| --- | --- |
| `lfs_file_open(fs, file, path)` | 按名字精确查找；不存在则创建一个 inode 和一个数据块。已存在文件不会被截断，`offset` 置 0，`length` 来自目录项 |
| `lfs_file_close(fs, file)` | 校验句柄，把长度和块号回写目录项/inode，并同步全部元数据 |
| `lfs_file_read(fs, file, buf, size)` | 从 `offset` 读取，最多 `min(size, length - offset)` 字节；EOF 返回 0；失败返回 -1 且不推进 `offset` |
| `lfs_file_write(fs, file, buf, size)` | 从 `offset` 写入，最多写到块尾；成功返回写入字节数，可能短写；失败返回 -1 且不改变 `offset`/`length` |
| `lfs_file_rewind(fs, file)` | 把 `offset` 置 0 |
| `lfs_file_seek(fs, file, offset, whence)` | `whence`：0=`SET`、1=`CUR`、2=`END`（`length + offset`）；结果必须落在 `[0, length]`，否则返回 -1 且不改变 `offset` |
| `lfs_file_dump(fs, file)` | 打印句柄的 `block_id`、`length`、`offset` |
| `lfs_file_remove(fs, path)` | 从有效链摘除目录项，释放数据块和 inode，再挂回空闲链；文件不存在或元数据非法时返回 -1 |
| `lfs_file_rename(fs, old, new)` | 改名；旧名必须存在，新名必须不存在；新旧名相同时若文件存在则成功；不覆盖目标 |

### 返回值与错误规则

- `read`/`write` 成功时返回传输字节数，失败返回 -1；其余文件 API 成功返回 0。
- 在句柄有效的前提下，`size == 0` 的 `read`/`write` 直接返回 0。
- 当 `offset == block_size` 时，非零长度的 `write` 返回 -1，而不是短写为 0。
- 写入达到块容量后，调用方需要根据返回值处理未写入的数据。
- 失败的 `read`/`write`/`seek` 不会改变句柄的 `offset`；失败的 `write` 也
  不会改变 `length`。
- 除“实例已挂载”这类前置条件失败外，`lfs_format`/`lfs_mount` 失败后会清理
  内存表并保持未挂载；`lfs_unmount` 即使同步失败也会释放内存表，失败后实例
  同样处于未挂载状态。`lfs_format` 在失败前可能已经写入了部分设备内容。
- 由于 `lfs_unmount` 失败后内存表已被释放，不能在同一实例上重试同步，只能
  重新挂载（或在卸载前通过 `lfs_file_close` 主动同步）。

### 持久化时机

- `lfs_file_write` 立即把修改后的整块文件数据写入设备，并更新内存中的目录项
  长度；但目录项、inode、位图和超级块要等到同步时才落盘。
- `lfs_file_close` 和 `lfs_unmount` 调用 `_sync_lfs`：依次写块位图、inode
  位图、inode 表、目录表，重算超级块 CRC，最后写超级块。
- `lfs_file_remove`、`lfs_file_rename` 只修改内存，不主动同步；在下一次
  close/unmount 时持久化。
- 没有独立的 `sync` API，也没有日志或崩溃恢复。数据块已写入但元数据尚未
  同步时崩溃，可能留下不可达块或长度不一致。

### 一次典型调用的数据流

1. `lfs_format`：校验配置和布局，在内存中建立超级块、位图和两张表，把数据区
   写成零块，然后逐块写回元数据并释放内存表。
2. `lfs_mount`：读取块 0 并校验，再按超级块中的块号逐块读入位图、inode 表和
   目录表，并从位图重算空闲计数。
3. `lfs_file_open`：在有效目录链中查找；未命中时 `_create_file` 从位图分配
   inode/数据块，清零新数据块，初始化目录项和 inode，并把它插入有效链。
4. `lfs_file_read` / `lfs_file_write`：按 `block_id` 读写单个数据块；写操作
   采用“读整块 → 修改 → 写整块”，立即写设备并更新内存中的目录项长度。
5. `lfs_file_close` / `lfs_unmount`：重算超级块 CRC，写回位图、inode 表、
   目录表，最后写超级块。

### 完整示例：内存块设备

下面的程序不依赖宿主文件，用一个静态数组模拟 128 个 1024 B 块，完成格式化、
挂载、写入、关闭、重新打开和读取：

```c
#include "lfs.h"

#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE 1024u
#define BLOCK_COUNT 128u

static unsigned char device[BLOCK_SIZE * BLOCK_COUNT];

static int dev_read(void *buffer, uint32_t block_id)
{
    if (!buffer || block_id >= BLOCK_COUNT) {
        return -1;
    }
    memcpy(buffer, device + (size_t)block_id * BLOCK_SIZE, BLOCK_SIZE);
    return 0;
}

static int dev_write(const void *buffer, uint32_t block_id)
{
    if (!buffer || block_id >= BLOCK_COUNT) {
        return -1;
    }
    memcpy(device + (size_t)block_id * BLOCK_SIZE, buffer, BLOCK_SIZE);
    return 0;
}

int main(void)
{
    lfs_config_t cfg = {
        .read = dev_read,
        .write = dev_write,
        .block_size = BLOCK_SIZE,
        .block_count = BLOCK_COUNT,
    };
    lfs_t fs = {0};
    lfs_file_t file = {0};
    const char message[] = "hello, lfs";
    char buffer[sizeof(message)] = {0};

    if (lfs_format(&fs, &cfg) != 0 || lfs_mount(&fs, &cfg) != 0) {
        return 1;
    }
    if (lfs_file_open(&fs, &file, "hello") != 0 ||
        lfs_file_write(&fs, &file, message, sizeof(message)) !=
            (int)sizeof(message) ||
        lfs_file_close(&fs, &file) != 0) {
        lfs_unmount(&fs);
        return 1;
    }

    memset(&file, 0, sizeof(file));
    if (lfs_file_open(&fs, &file, "hello") != 0 ||
        lfs_file_read(&fs, &file, buffer, sizeof(buffer)) !=
            (int)sizeof(buffer) ||
        strcmp(buffer, message) != 0) {
        lfs_unmount(&fs);
        return 1;
    }

    printf("%s\n", buffer);
    lfs_file_close(&fs, &file);
    lfs_unmount(&fs);
    return 0;
}
```

将代码保存为 `/tmp/lfs_example.c`，在仓库根目录编译并运行：

```sh
cc -std=c17 -Wall -Wextra -Iinclude /tmp/lfs_example.c \
    build/liblfs.a -o /tmp/lfs_example
/tmp/lfs_example
# hello, lfs
```

完整的 POSIX 文件镜像回调实现见 `src/main.c`。新文件第一次读取返回 0，
随后可直接写入；写入长度受单块容量限制。

## 常见问题

**`lfs_file_open` 会截断已有文件吗？**
不会。已存在文件会保留原长度，句柄的 `offset` 置 0。要清空内容，需要删除
后重新创建（或写入新内容并接受旧尾部仍可能存在）。

**如何只打开已有文件，不自动创建？**
当前不支持。`lfs_file_open` 没有打开模式参数，总是“存在则打开，不存在则
创建”；也没有独立的 `stat` 或存在性检查 API。

**写满一个块后继续写会怎样？**
当 `offset == block_size` 时，非零长度的 `lfs_file_write` 返回 -1。如果本次
写入跨越块尾，会先返回实际写入的字节数（短写），调用方需要处理剩余数据。

**删除一个仍打开的文件会怎样？**
删除操作会成功并释放其数据块和 inode。旧句柄随后失效，后续操作返回 -1。
库没有打开计数或引用保护。

**修改什么时候真正落盘？**
文件数据在 `lfs_file_write` 时立即写设备；目录项、inode、位图和超级块在
`lfs_file_close` 或 `lfs_unmount` 时写回。`remove`/`rename` 不会立即同步。

**`lfs_format` 之后为什么还要 `lfs_mount`？**
`lfs_format` 只负责把新布局写到设备并释放内存表，完成后实例仍未挂载；
`lfs_mount` 才把元数据读入内存并建立可操作的实例。

**能同时挂载多个设备吗？**
库本身没有全局状态，各 `lfs_t` 相互独立；但回调没有上下文参数，应用需要
自行通过全局/静态状态路由到对应设备，并自行保证线程安全。

## 已知问题与限制

### 文件模型

- 每个文件只分配一个数据块，逻辑长度上限为 `block_size`；不支持跨块增长、
  稀疏文件或多块扩展。
- 没有 `O_TRUNC`、`O_APPEND`、只读等打开模式。打开已有文件不截断；从头部
  覆盖写入较短内容也不会缩短文件，旧尾部会保留。
- `seek` 只能落在 `[0, length]`，不支持定位到 EOF 之后。
- 没有截断、追加或多句柄引用计数。

### 目录与句柄

- 仅根目录下的扁平文件名，最长 64 字节；名字按字节比较，不解析路径。
- 没有权限、时间戳、符号链接、子目录、缓存或并发访问控制。
- 库不跟踪打开句柄；删除打开中的文件会成功，之后旧句柄失效。
- `lfs_file_rename` 在目标已存在时返回 -1，与 POSIX 的替换语义不同。
- 重命名打开中的文件本身是允许的，因为句柄引用目录项。

### 一致性与持久化

- 没有日志、崩溃恢复或 fsck。同步先写元数据表、最后写超级块，中途失败或
  掉电仍可能留下不一致镜像。
- 挂载只校验超级块 CRC、布局和基本范围，并重算空闲计数；不校验目录项、
  inode 和位图之间的完整一致性。
- 超级块 CRC 只保护超级块本身，位图、inode 表和目录表没有校验和。
- 没有独立的 `sync` API；`remove`/`rename` 的修改要等到下一次 close/unmount。
- `lfs_unmount` 在同步失败时仍释放内存表，无法在当前实例上重试同步。
- `lfs_format` 不是原子操作；清零数据区或写元数据失败时可能留下部分格式化
  的镜像。

### 平台与性能

- 磁盘结构按宿主结构体布局落盘，没有显式端序转换，仅同 ABI 可读。
- 设备回调没有上下文指针，必须一次读写完整块并负责持久化。
- 每次读写都会分配整块缓冲区并读写整个块；`close`/`unmount` 回写全部元数据；
  格式化会逐块清零整个数据区。设计目标是教学清晰度，而非吞吐或延迟。
- `tests/` 覆盖核心路径、布局边界和故障注入，但不是穷尽测试，也没有并发、
  掉电或多平台 ABI 测试。

## 后续方向

- 增加多块文件增长（需要新的磁盘版本、块映射和边界校验）。
- 改为显式小端编解码，避免直接持久化宿主结构体。
- 增加日志/崩溃恢复、fsck 和更完整的目录/位图一致性校验。
- 增加截断、打开模式、独立的 sync API 和打开句柄跟踪。
- 拆分模块，扩充故障注入、并发和压力测试，并增加多平台 CI。

## 代码风格

仓库根目录的 `.clang-format` 约定：Google 风格、4 空格缩进、80 列、
控制语句必须使用花括号。
