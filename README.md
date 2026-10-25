# build-own-fs

C17 单目录教学文件系统。应用只需包含 `lfs.h`，提供整块 `read/write`
回调和块大小、块数量。所有文件直接放在根目录，支持创建、读写、定位和信息打印。

## 构建与运行

需要 CMake 3.16+ 和 C17 编译器；计数器示例及部分测试使用 POSIX 接口。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure

./build/build_own_fs build/counter.img
./build/build_own_fs build/counter.img
./build/build_own_fs build/counter.img
```

三次运行分别输出 `boot_count: 1`、`boot_count: 2`、`boot_count: 3`，随后打印
文件系统信息。不传路径时使用当前目录的 `lfs.img`。

[src/main.c](src/main.c) 是完整示例：通过文件内的 `static FILE*` 绑定镜像设备，
仅在镜像不存在时创建并格式化；已有镜像大小错误或挂载失败时报告错误，不自动覆盖。
示例使用4096字节块、128块，镜像大小512 KiB。

## 最小配置与接口

```c
#include "lfs.h"

static int device_read(void* buffer, uint32_t block);
static int device_write(const void* buffer, uint32_t block);

static lfs_t fs;
static lfs_file_t file;
static const struct lfs_config cfg = {
    .read = device_read,
    .write = device_write,
    .block_size = 4096,
    .block_count = 128,
};
```

回调由应用实现，每次传输完整块，返回0成功、任何非零值失败。配置中没有设备状态
指针、擦除、缓存或同步设置；每组回调自行绑定一个活动设备。不同设备应提供不同的
回调函数组。同一设备不可同时通过多个可写实例修改，否则内存位图可能失配。

| 操作 | 调用形式 |
| --- | --- |
| 格式化 | `lfs_format(&fs, &cfg)` |
| 挂载、卸载 | `lfs_mount(&fs, &cfg)`、`lfs_unmount(&fs)` |
| 打开、关闭 | `lfs_file_open(&fs, &file, "boot_count", flags)`、`lfs_file_close(&fs, &file)` |
| 读、写 | `lfs_file_read(&fs, &file, buffer, size)`、`lfs_file_write(&fs, &file, buffer, size)` |
| 定位 | `lfs_file_seek(&fs, &file, offset, whence)` |
| 文件大小 | `lfs_stat(&fs, "boot_count", &info)` |
| 文件系统信息 | `lfs_dump(&fs)` |

- 实例和句柄必须零初始化，存活期间不能复制；先关闭全部文件，再卸载。
- 格式化要求实例未挂载，清空目录并重建元数据，不隐式挂载。
- 名字接受 `"boot_count"` 或 `"/boot_count"`，按字节比较，最长58字节。
  拒绝空名字、`.`、`..`、重复前导斜杠、尾部斜杠和子路径。
- 打开标志为 `LFS_O_RDONLY/WRONLY/RDWR`，可组合 `LFS_O_CREAT/EXCL`。
  `EXCL` 必须配合 `CREAT`。多个读句柄可共存，写句柄独占同一文件。
- 每次文件操作检查句柄归属。所有新句柄从位置0开始，读写推进各自位置。
- 状态操作返回0或负错误码；读写返回实际字节数或负错误码，EOF返回0。
  中途失败、空间不足或容量限制可能返回部分字节数，调用者必须检查。
- `seek` 成功返回新位置，允许 `[0, 文件大小]`，支持 `LFS_SEEK_SET/CUR/END`。
  回到开头使用 `lfs_file_seek(&fs, &file, 0, LFS_SEEK_SET)`。
- 不提供文件删除、子目录、截断、追加、只读挂载或显式同步；清空设备需要重新格式化。

## dump 输出

`lfs_dump` 向标准输出打印版本、块大小、总块数、最大文件大小、各元数据区域和
数据区的位置与块数、有效块和inode的使用统计、打开句柄数、失败状态及文件列表。

```text
format_version: 3
block_size: 4096
total_blocks: 128
max_file_size: 4243456
...
open_handles: 0
failed: no
files (name inode size):
  boot_count 1 4
```

块统计包含元数据块，inode统计包含根目录，均排除位图填充位。
`dump` 只读取目录和inode，不读取正文、修改设备或执行修复。
无效实例、读取失败或输出失败会返回错误码；成功返回0。

## 内部结构与限制

核心静态库为 `lfs`；镜像回调仅在示例中，内存回调仅在测试中。

| 模块 | 职责 |
| --- | --- |
| `src/lfs.c` | 公开文件接口和句柄生命周期 |
| `src/lfs_dir.c` | 根目录条目的查找和追加 |
| `src/lfs_inode.c` | inode、直接块和一级间接块读写 |
| `src/lfs_storage.c` | 格式化、挂载和位图分配回收 |
| `src/lfs_device.c` | 整块I/O、小端编码和CRC32 |
| `src/lfs_dump.c` | 只读信息打印 |

v3布局为 `超级块 | 块位图 | inode位图 | inode表 | 数据区`，显式小端编码，
超级块带CRC32。旧镜像返回版本错误，需要显式重新格式化；旧API不兼容。

inode总数为 `max(2, block_count / 8)`，根目录占用一个inode；文件数量还受根目录
容量和可用空间限制。每个inode固定64字节，含12个直接块号和1个一级间接块号。
最大文件大小为 `(12 + block_size / 4) * block_size`；512字节块对应70 KiB。
目录项固定64字节，保存inode编号、类型和最多58字节名字。
块大小至少512字节，必须为64的倍数；格式化前检查布局能否放入设备。

写回调负责覆盖写和所需的持久化保证。示例每次块写入后执行 `fflush/fsync`。
关闭文件只释放句柄，不是延迟提交点。写回调失败后禁止继续修改，允许关闭文件；
卸载仍释放资源并返回I/O错误。部分元数据修改后的其他错误也可能触发失败状态。

单线程，不支持日志、崩溃恢复或多块事务回滚，也不实现完整fsck。
挂载校验布局、超级块和位图保留位；访问时校验条目与块指针，但不保证发现所有损坏。

## 格式与测试

代码使用 Google 风格、4空格缩进和控制语句花括号，配置见 [.clang-format](.clang-format)。
clangd可读取该配置；安装独立clang-format后可运行：

```sh
clang-format -i include/*.h src/*.c src/*.h tests/*.c tests/*.h
```

CTest覆盖挂载、分配、文件、名字规则、故障注入、dump及真实镜像的计数器持久化。
内存检查：

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DLFS_SANITIZE=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```
