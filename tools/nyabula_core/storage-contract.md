# Storage 当前契约与持久化边界

Storage由QuickJS和WAMR共享Broker执行。每个插件的`storage_root/data`为私有键目录；通过`start-installed`调度的版本共用插件根目录。直接`run-package`及`promote`候选自测使用包目录作为Storage根，不应把候选自测数据视为生产共享数据。

## 已实现的资源限制

- 单值上限：`NYABULA_CORE_STORAGE_VALUE_LIMIT`，默认4096字节。
- 所有值的逻辑总字节上限：`NYABULA_CORE_STORAGE_BYTES_LIMIT`，默认65536字节。
- 键数量上限：`NYABULA_CORE_STORAGE_KEYS_LIMIT`，默认64。

启用`NYABULA_CORE_STATE_SQLITE`时，在同一数据库事务内统计已持久化的键数和字节数，再更新目标值；关闭时扫描实际目录。两种后端均不依赖内存计数。覆盖同一个键时扣除其旧长度，按新长度核算；零长度键仍占用一个名额。读写和预算检查受同一进程内的Broker锁保护。不同插件根目录独立核算。

超出单值上限返回EFBIG；超出字节或键数上限返回EDQUOT。配额拒绝发生在打开目标写入之前，旧值保持不变。此处约束的是逻辑数据量，文件系统元数据、分配簇和包版本空间不计入该数值。

读取在打开前检查lstat，打开后检查fstat；只允许有界普通文件。完整处理短读和EINTR，提前EOF报告EIO，文件增长报告EFBIG；错误路径清空输出指针和长度。显式lstat是必要的：当前NuttX sim的O_NOFOLLOW链路曾实测跟随符号链接。

目录必须由Core独占管理。进程内锁和lstat检查不构成对其他具有原生文件系统权限进程的隔离；最终跨进程架构仍需由唯一Broker管理文件访问。

## 事务后端与兼容迁移

`NYABULA_CORE_STATE_SQLITE`使用上游SQLite 3.45.1，不自行实现事务日志。数据库位置由`NYABULA_CORE_STATE_DATABASE`配置。当前使用DELETE journal、FULL同步、禁用WAL和mmap，以进程内唯一Broker锁保护unix-none VFS；禁止其他进程打开同一数据库，Kernel配置暂不启用本后端。

授权、撤销、health及包marker也使用此库；current与last-good在一个事务内更新。Storage首次写入某插件目录时，在事务中导入原有普通键文件并记录导入标记，后续写入不再扫描旧文件。旧文件保留不修改，仅作迁移前备份；降级到旧固件将看到旧快照，而非数据库最新值。不得直接切回旧后端作为无损回滚。

数据库无对应记录时，当前保留旧文件读取兼容。数据库损坏返回错误，不退回旧数据。删除数据库会丢失新状态并重新触发旧文件兼容，故数据库及备份必须由可信Core独占管理；本机制不是对拥有原生文件写权限者的防篡改边界。

已在NuttX sim hostfs验证分配失败和write/truncate/fsync错误；逐次重开数据库检查双键更新完整性。此证据不等于真实介质断电耐久；FAT、块设备flush和突然掉电仍需单独验证。

## 关闭事务后端时的限制

未启用SQLite时写入仍为O_TRUNC。磁盘满、写入错误或断电时不能保证旧值保留；只有配额拒绝和参数/权限拒绝保证写入前返回。

当前NuttX `fs/vfs/fs_rename.c`的mountptrename在调用文件系统rename之前先unlink目标，FAT实现依赖此行为。因此“写临时文件、fsync、rename覆盖”不能被宣称为原子更新；这一限制同样需要审查授权库、撤销库和包marker的替换操作。

本后端绕开rename覆盖，不修改NuttX VFS，也不宣称修复了其他调用者的rename语义。
