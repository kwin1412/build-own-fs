# build-own-fs

一个用于学习文件系统原理的极简 C17 文件系统原型。应用通过回调提供整块设备
读写，文件系统负责超级块、位图、inode 表、目录表和根目录下的文件操作。

> **状态：v1 原型。** 磁盘格式版本为 `LFS_VERSION = 1`。当前实现仍偏重演示
> 基本流程，持久化语义和错误处理尚不完整，接口与磁盘格式可能变化，请勿用于
> 保存真实数据。

## 功能概览

- 固定大小的块设备，块大小和块数量由挂载配置指定
- 设备读写通过函数回调注入，核心库不直接操作宿主文件
- 支持格式化、挂载、卸载和元数据打印
- 挂载时校验魔数、版本、CRC32 以及块大小/块数量
- 根目录下的扁平命名空间：打开文件时按名字查找，不存在则创建
- 支持文件打开、关闭、读、写、重定位、重命名、删除和句柄打印
- 使用内存中的块位图、inode 位图、inode 表和目录表，关闭/卸载时回写

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
└── fs.img                 # 示例镜像（4 MiB，运行示例会覆盖）
```

`CMakeLists.txt` 使用 `GLOB_RECURSE` 收集 `src/*.c` 和 `include/*.h`，因此新增
源文件无需手动修改构建脚本。

## 构建

依赖：CMake ≥ 3.16、支持 C17 的 C 编译器。无第三方库。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

产物：

- `build/liblfs.a`：静态库
- `build/build_own_fs`：示例程序

当前仓库没有自动化测试，`CMakeLists.txt` 也未配置 CTest，因此无需
`ctest` 步骤。

## 运行示例

`src/main.c` 实现了一个 POSIX 镜像设备，并在镜像中维护 `boot_count` 计数器：

1. 每次启动强制格式化（`FORCE_FORMAT` 为 1）；
2. 挂载镜像；
3. 打开或创建 `boot_count`，读取旧值；
4. 值加一后写回并关闭；
5. 打印超级块和目录，然后卸载。

设备回调以 `"r+b"` 打开 `./fs.img`，**不会创建文件**，因此运行前需要预置一个
至少 `block_size × block_count = 4096 × 1024 = 4 MiB` 的镜像。建议在临时目录
中运行，避免覆盖仓库内的镜像：

```sh
mkdir -p /tmp/lfs-demo
cd /tmp/lfs-demo
truncate -s 4M fs.img
/path/to/build/build_own_fs
```

典型输出（省略部分字段）：

```text
boot_count: 0
super block:
  magic: 0x004c4653
  version: 1
  block_size: 4096
  total_block_count: 1024
  free_block_count: 1024
  ...
  block_bmp_block_id: 1
  inode_bmp_block_id: 2
  inode_table_block_id: 3
  dir_table_block_id: 5
  valid_dir_entry_id: 0
  free_dir_entry_id: 1
  crc32: 0x0756a9ee
directory:
  [0] name="boot_count" inode_id=0 size=0 block_id=25 next_inode_id=4294967295
boot_count: 1
```

注意最后目录中 `size=0`：当前写入只修改句柄内的长度，没有回写目录项的
`length`，所以计数器实际上不会跨进程持久化（详见“已知问题”）。

## 磁盘格式

### 整体布局

块 0 固定为超级块，随后依次是块位图、inode 位图、inode 表和目录表，剩余块
构成数据区：

```text
+-------------+-------------+-------------+-------------+-------------+------------+
| super block | block bitmap| inode bitmap| inode table | dir table   | data blocks|
+-------------+-------------+-------------+-------------+-------------+------------+
      块 0          连续块        连续块        连续块        连续块        剩余块
```

默认配置（4096 B × 1024 块）下的实际分布：

| 块号 | 用途 | 占用 |
| --- | --- | --- |
| 0 | 超级块 | 1 块 |
| 1 | 块位图 | 1 块 |
| 2 | inode 位图 | 1 块 |
| 3–4 | inode 表（1024 × 8 B） | 2 块 |
| 5–24 | 目录表（1024 × 80 B） | 20 块 |
| 25–1023 | 数据区 | 999 块 |

各区域块数由以下公式计算（`DIV_ROUND_UP` 为向上取整）：

```text
块位图块数   = ceil(total_block_count / (block_size × 8))
inode 位图块数 = ceil(total_inode_count / (block_size × 8))
inode 表块数   = ceil(total_inode_count × sizeof(inode_t) / block_size)
目录表块数     = ceil(total_block_count × sizeof(dir_entry_t) / block_size)
```

格式化时把元数据区各块在块位图中标记为已占用，并把数据区全部写成零块。

### 超级块（`super_block_t`，56 B）

| 字段 | 含义 |
| --- | --- |
| `magic` | `0x004C4653`（`"LFS"`） |
| `version` | 格式版本，当前为 1 |
| `block_size` | 块大小，最小 1024 B |
| `total_block_count` | 设备总块数 |
| `free_block_count` | 空闲块计数（当前不维护） |
| `total_inode_count` | inode 总数，格式化时取块总数 |
| `free_inode_count` | 空闲 inode 计数（当前不维护） |
| `block_bmp_block_id` | 块位图起始块 |
| `inode_bmp_block_id` | inode 位图起始块 |
| `inode_table_block_id` | inode 表起始块 |
| `dir_table_block_id` | 目录表起始块 |
| `valid_dir_entry_id` | 有效目录项链表头，`0xFFFFFFFF` 表示空 |
| `free_dir_entry_id` | 空闲目录项链表头 |
| `crc32` | 对除本字段外全部超级块字节计算的 CRC-32 |

### inode（`inode_t`，8 B）

| 字段 | 含义 |
| --- | --- |
| `block_id` | 文件数据起始块 |
| `next_inode_id` | 预留的 inode 链指针，当前恒为 `0xFFFFFFFF` |

### 目录项（`dir_entry_t`，80 B）

| 字段 | 含义 |
| --- | --- |
| `inode_id` | 指向 inode 表的下标 |
| `length` | 文件逻辑长度（字节） |
| `next_entry_id` | 链表中的下一个目录项 |
| `name[65]` | 文件名，最长 64 个字符，含结尾 `\0` |

目录项组成两条单链表：

- **有效链**：从 `valid_dir_entry_id` 开始，`_find_dir_entry` 顺序查找；
- **空闲链**：从 `free_dir_entry_id` 开始，格式化时把所有项串成
  `0 → 1 → … → 总块数-1 → INVALID`。

创建文件时从空闲链取头节点，插入有效链头部；删除文件时把节点挂回空闲链。

### 位图

`bmp_t` 为 `uint8_t`，每个 bit 表示一个块或一个 inode，`1` 表示已占用，
`0` 表示空闲；位序为字节内从低位到高位。分配采用首次适配：从头扫描第一个
为 0 的位。

> **可移植性说明：** 磁盘结构直接按宿主机的结构体布局写入设备，没有做显式的
> 小端编码或字段序列化。仅在相同 ABI（端序、对齐、填充一致）的平台上可读。

## 内存结构与接口

```c
typedef int block_dev_read(void *buffer, uint32_t block_id);
typedef int block_dev_write(const void *buffer, uint32_t block_id);

typedef struct {
    block_dev_read *read;
    block_dev_write *write;
    uint32_t block_size;   // 格式化时使用
    uint32_t block_count;  // 格式化时使用
} lfs_config_t;

typedef struct {
    uint32_t block_id;  // 文件数据起始块
    uint32_t length;    // 打开时快照的文件长度
    uint32_t offset;    // 当前读写位置
} lfs_file_t;
```

回调约定：每次传输一个完整块，成功返回 0，任何非零值表示失败。`lfs_t`
保存回调、超级块副本以及四张内存表的指针，由 `lfs_mount` 分配、
`lfs_unmount` 释放；调用方应把 `lfs_t` 和 `lfs_file_t` 零初始化。

### 文件系统 API

| 函数 | 说明 | 返回值 |
| --- | --- | --- |
| `lfs_format(fs, cfg)` | 初始化超级块和元数据，清零数据区并写回设备 | 当前总是 0 |
| `lfs_mount(fs, cfg)` | 读入超级块和元数据表，校验魔数、CRC、版本和配置 | 0 或非零错误码 |
| `lfs_unmount(fs)` | 回写元数据并释放内存表 | 当前总是 0 |
| `lfs_dump(fs)` | 打印超级块和有效目录链，不修改设备 | 0 或 -1 |

### 文件 API

| 函数 | 说明 |
| --- | --- |
| `lfs_file_open(fs, file, path)` | 按名字查找；不存在则创建（分配一个 inode 和一个数据块）。无打开模式参数 |
| `lfs_file_close(fs, file)` | 触发一次元数据同步 |
| `lfs_file_read(fs, file, buf, size)` | 从当前 offset 最多读取 `min(size, length - offset)` 字节，不推进 offset |
| `lfs_file_write(fs, file, buf, size)` | 在 `[offset, length)` 范围内写入，不推进 offset |
| `lfs_file_rewind(fs, file)` | 把 offset 置 0 |
| `lfs_file_seek(fs, file, offset, whence)` | `whence`：0=`SET`、1=`CUR`、2=`END`；结果须落在 `[0, length]` |
| `lfs_file_dump(fs, file)` | 打印句柄的 `block_id`、`length`、`offset` |
| `lfs_file_remove(fs, path)` | 释放数据块、inode，并把目录项挂回空闲链 |
| `lfs_file_rename(fs, old, new)` | 直接覆盖目录项中的名字 |

### 一次典型调用的数据流

1. `lfs_format`：在内存中建立超级块、位图和两张表，把数据区写成零块，
   然后写回全部元数据并释放内存表。
2. `lfs_mount`：读取块 0 并校验，再按超级块中的块号读入位图、inode 表和
   目录表。
3. `lfs_file_open`：在有效目录链中查找；未命中时 `_create_file` 从位图分配
   inode/块，初始化目录项和 inode，并把它插入有效链。
4. `lfs_file_read` / `lfs_file_write`：直接按 `block_id` 加块内偏移访问连续
   数据块；写操作立即写设备，不经过关闭提交。
5. `lfs_file_close` / `lfs_unmount`：把超级块和四张元数据表整体回写设备。

### 嵌入示例

```c
#include "lfs.h"

static int dev_read(void *buffer, uint32_t block_id);
static int dev_write(const void *buffer, uint32_t block_id);

void demo(void) {
    lfs_config_t cfg = {
        .read = dev_read,
        .write = dev_write,
        .block_size = 4096,
        .block_count = 1024,
    };
    lfs_t fs = {0};
    lfs_file_t file = {0};
    uint32_t value = 0;

    if (lfs_mount(&fs, &cfg) != 0) {
        lfs_format(&fs, &cfg);
        lfs_mount(&fs, &cfg);
    }

    if (lfs_file_open(&fs, &file, "boot_count") == 0) {
        /* 当前实现要求 offset < length 才能读写，新文件长度为 0 */
        lfs_file_read(&fs, &file, &value, sizeof(value));
        lfs_file_rewind(&fs, &file);
        lfs_file_write(&fs, &file, &value, sizeof(value));
        lfs_file_close(&fs, &file);
    }

    lfs_unmount(&fs);
}
```

完整的设备回调实现见 `src/main.c`。注意受上述限制影响，新文件第一次 `write`
  在当前实现中会失败，这段代码只用于展示调用顺序。

## 已知问题与限制

当前是教学原型，下列限制会直接影响使用：

- **持久化长度未回写。** `lfs_file_write` 只更新 `lfs_file_t.length`，不更新
  目录项的 `length`；重新挂载后文件长度仍是旧值。示例计数器因此不会真正累加。
- **文件不能增长。** 创建文件时只分配一个数据块，没有块扩展逻辑；读写把数据
  块视为从 `block_id` 开始的连续区域，越过已分配块会写到其他数据块或元数据；
  读写要求 `offset < length`，长度为 0 的新文件无法读写。
- **句柄语义不完整。** `read`/`write` 不推进 `offset`，返回 0 而不是传输字节
  数；`write` 还会把句柄的 `length` 直接改成请求的 `size`；`open` 不初始化
  `offset`；`SEEK_END` 忽略传入的偏移；没有打开模式、截断、追加或“读改写”
  保护。
- **写缓冲区未清零。** 写入时使用 `malloc` 的整块缓冲区，块内未覆盖的部分为
  未初始化数据，会被一并写进设备。
- **删除不摘链。** `lfs_file_remove` 把目录项挂回空闲链，却没有从有效链移除，
  该名字仍可被查找；且只释放第一个数据块。
- **重命名无校验。** 新名字长度不受限，可能越界写；也不检查重名。
- **空闲计数不准确。** `free_block_count`、`free_inode_count` 只在格式化时
  置为总量，分配和释放都不更新。
- **元数据回写不完整。** `_sync_lfs` 忽略写回调的返回值；inode 表和目录表的
  多块回写对指针做了按元素类型的偏移，首块之外的位置不可靠；超级块回写把
  56 字节结构体指针直接交给整块写回调，存在越界读取。
- **错误处理薄弱。** `lfs_format` 无论内部是否失败都返回 0；部分失败路径未
  清理；`lfs_mount` 首次读失败时会泄漏临时缓冲区。
- **设备要求。** 回调必须一次读写完整块；示例实现每次操作都 `fopen`/`fclose`，
  忽略 `fread`/`fwrite` 的返回值，镜像必须预先存在且不小于
  `block_size × block_count`。示例中 `FORCE_FORMAT` 恒为 1，每次运行都会重新
  格式化并覆盖 `./fs.img`。
- **无高级特性。** 不支持子目录、权限、时间戳、符号链接、缓存、并发访问、
  日志、崩溃恢复或 fsck；当前也没有自动化测试。
- **平台相关。** 元数据按宿主结构体布局落盘，没有显式端序转换。

## 后续方向

- 拆分模块（设备、分配、inode、目录、dump）并补齐失败路径测试
- 完善文件增长与长度持久化，明确 `read`/`write` 的偏移和返回值语义
- 改为显式小端编解码，避免直接持久化宿主结构体
- 增加基本的目录/位图一致性校验与恢复手段

## 代码风格

仓库根目录的 `.clang-format` 约定：Google 风格、4 空格缩进、80 列、
控制语句必须使用花括号。
