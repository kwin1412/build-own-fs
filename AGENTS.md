# Codex 工程指引

这是 C17/CMake 单目录教学文件系统，默认使用中文交流。README 描述v3格式、接口、
示例和限制；修改接口或格式时同步更新文档与测试。

## 结构与边界

- `include/lfs.h`：唯一公开头文件，包含配置、句柄、错误码和最小文件API。
- `include/lfs_struct.h`：内部磁盘格式常量；宿主结构不能直接持久化。
- `src/lfs.c`：API与句柄；`lfs_dir.c`：根目录条目；`lfs_inode.c`：inode与数据。
- `src/lfs_storage.c`：挂载、格式与分配；`lfs_device.c`：设备与编码；`lfs_dump.c`：信息打印。
- `src/lfs_internal.h`：内部类型和接口；`src/main.c`：POSIX镜像计数器示例。
- `tests/`：内存设备辅助代码和七组CTest；镜像测试通过独立进程运行示例。

核心静态库 `lfs` 不管理宿主文件；设备状态由应用静态变量绑定回调。
配置只包含整块read/write和块大小/数量，不添加上下文指针、同步或缓存选项。
仅支持根目录文件，不增加目录操作、文件删除、截断、追加或rewind包装。
内部回收函数仅用于失败清理。测试链接库，不直接包含实现 `.c`。

## 构建与验证

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DLFS_SANITIZE=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```

示例：`./build/build_own_fs build/counter.img`，不存在时创建，已有镜像只尝试挂载。
新增模块或测试时更新CMake。构建产物位于忽略的 `build/` 或 `build-*/`。

## 实现约定

- 遵循根目录 `.clang-format`：Google风格、4空格缩进、80列、控制语句花括号。
- 实例和句柄零初始化，不复制存活对象；每次文件操作校验所属实例。
- 回调传输完整块，0成功、任何非零值失败；写回调负责持久化，关闭不提交数据。
- 显式小端编解码，检查溢出与边界；预留元数据块、根inode和位图填充位。
- 文件新块清零；同文件多读共享，写句柄独占。
- 写失败后拒绝修改；关闭正常释放句柄，卸载释放资源并报告实例失败状态。
- 位图写失败恢复内存位并保留调用者输出；不承诺已落盘修改回滚。
- dump只读目录和inode，不读正文、不写设备、不修复；输出错误也须传播。
- 无日志、崩溃恢复和完整fsck。不要把局部校验描述为全面一致性保障。
- 功能变化覆盖相关边界与失败路径，沿用内存设备和CHECK断言风格。

完成后报告变更与验证结果。比较差异前确认Git可用，不根据 `.git` 目录存在作判断。
