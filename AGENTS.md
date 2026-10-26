# Codex 工程指引

这是 C17/CMake 单目录教学文件系统，默认使用中文交流。当前磁盘格式为 v1
（`LFS_VERSION`）。README 是格式、接口、示例、限制和已知问题的权威说明；
修改接口、磁盘格式或用户可见行为时同步更新 README。

## 结构与边界

- `include/lfs.h`：唯一公开头文件，包含配置、句柄和文件系统/文件 API。
- `include/lfs_struct.h`：磁盘结构（超级块、inode、目录项）与格式常量。
- `src/lfs.c`：当前全部实现，包含挂载、格式化、分配、目录、inode、文件和 dump。
- `src/main.c`：基于 POSIX 镜像的计数器示例。
- `CMakeLists.txt`：显式列出 `src/lfs.c` 和头文件生成静态库 `lfs`；`src/main.c`
  单独生成 `build_own_fs`。测试位于 `tests/`，通过 CTest 运行并链接 `lfs`。
- 已提供 `BUILD_TESTING`（默认开启）和 `LFS_SANITIZE` 选项；测试不得直接编译
  `src/lfs.c`。

核心静态库 `lfs` 不管理宿主文件；设备由应用通过回调提供。配置只包含整块
read/write 回调和块大小/块数量，不添加上下文指针、同步或缓存选项。
仅支持根目录下的扁平文件名，不增加子目录、路径解析、权限、链接、截断、追加、
并发或缓存功能。现有 API 已包含 remove/rename/rewind/seek，保持范围克制；
新增功能须先确认必要性并同步文档。
`_alloc_*`、`_free_*` 等辅助函数保持 `static`，不扩展公开接口。

## 构建与验证

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

ASan/UBSan：

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON -DLFS_SANITIZE=ON
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    ctest --test-dir build-asan --output-on-failure
```

示例程序把设备路径硬编码为 `./fs.img`，配置为 4096 B × 1024 块（4 MiB），
并通过 `"r+b"` 打开镜像，不会创建文件。验证时在临时目录预置镜像后运行：

```sh
mkdir -p /tmp/lfs-demo && cd /tmp/lfs-demo
truncate -s 4M fs.img
/path/to/build/build_own_fs
/path/to/build/build_own_fs
```

`src/main.c` 不再强制格式化：先挂载，失败时才格式化。连续运行两次可观察
`boot_count` 从 0→1、1→2；需要保留数据的镜像不要被失败的挂载触发格式化。
构建产物位于忽略的 `build/` 或 `build-*/`。

## 实现约定

- 遵循根目录 `.clang-format`：Google 风格、4 空格缩进、80 列、控制语句花括号。
- 实例和句柄由调用方零初始化；新增代码在操作前校验指针、边界和句柄状态，
  不复制存活对象。
- 回调一次传输完整块，0 成功、任何非零值失败；写回调负责持久化。文件数据在
  write 时直接落盘，close/unmount 负责回写元数据。
- 磁盘结构目前按宿主结构体布局直接落盘，没有显式端序转换，仅同 ABI 可读。
  演进格式时必须改为显式小端编解码、检查溢出和边界，提升 `LFS_VERSION`，
  并同步更新 README；不要无意改变现有布局。
- 位图约定：1 表示占用，0 表示空闲，字节内从低位到高位；数据块分配从数据区
  起点开始首次适配，inode 从 0 开始。格式化时把元数据块标为已占用。
- 文件创建只分配一个数据块；逻辑长度上限为 `block_size`，不支持跨块增长。
  读写使用 `block_id` 单块访问，写操作必须“读整块 → 修改 → 写整块”，并在
  分配/创建时清零数据块。
- 读写为 POSIX 风格：推进 `offset`，返回实际传输字节数；EOF 返回 0；错误返回
  -1。`write` 在容量不足时短写。`lfs_file_t.dir_entry_id` 用于把长度回写目录项；
  所有句柄操作都必须校验目录项/inode/块范围。
- 目录操作维护两条单链表：删除必须先从有效链摘除并清空目录项，再挂回空闲链；
  重命名拒绝已存在的目标。分配/释放同步维护空闲计数，挂载时从位图重算。
- 超级块 CRC 在每次 `_sync_lfs` 写超级块前重算；多块元数据必须逐块读写，
  使用字节指针偏移，先写元数据表、最后写超级块。
- dump 只读内存中的超级块、目录和 inode，不读文件正文、不写设备、不修复；
  检测到越界或成环时返回错误。
- 无日志、崩溃恢复和完整 fsck；不要把局部校验描述为全面一致性保障。
- 剩余限制（单数据块、无打开模式、同步非原子、无端序转换等）记录在 README
  的“已知问题与限制”中；修复时同步更新 README。新增功能应覆盖边界和失败路径；
  修改行为时补充 `tests/` 用例。
- 完成后报告变更与验证结果。比较差异前确认 Git 可用，不根据 `.git` 目录存在
  作判断。
