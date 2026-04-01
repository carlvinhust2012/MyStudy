定义了一批文件操作的api
https://github.com/carlvinhust2012/juicefs/blob/main/pkg/fs/fs.go#L160
单元测试，里面有各个接口的使用
https://github.com/carlvinhust2012/juicefs/blob/main/pkg/fs/fs_test.go#L35

type FileStat struct {
	name  string     // filename？
	inode Ino        // similar to linux ext4 inode
	attr  *Attr      // attribute
}

JuiceFS 的代码结构如下：
1. cmd 目录
功能：包含主要的命令行工具和不同平台的挂载实现。
入口文件：cmd/juicefs.go 是项目的入口文件，负责解析命令行参数并调用相应的功能模块。
go复制
package main

import (
    "github.com/juicedata/juicefs/pkg/cmd"
    "github.com/juicedata/juicefs/pkg/version"
    "github.com/urfave/cli/v2"
    "log"
    "os"
)

func main() {
    app := &cli.App{
        Name:    "juicefs",
        Usage:   "A POSIX file system built on Redis and S3",
        Version: version.Version,
        Commands: []*cli.Command{
            cmd.MountCommand,
            cmd.FormatCommand,
            cmd.GCCommand,
            cmd.InfoCommand,
            cmd.StatusCommand,
            cmd.WebDAVCommand,
            cmd.BenchCommand,
            cmd.ShellCommand,
        },
    }
    if err := app.Run(os.Args); err != nil {
        log.Fatal(err)
    }
}

2. pkg 目录
功能：包含项目的核心包，如元数据处理、对象存储接口、文件系统实现等。
pkg/fuse/fuse.go：提供抽象 FUSE 接口。
pkg/vfs：具体的 FUSE 接口实现，元数据请求会调用 pkg/meta 中的实现，读请求会调用 pkg/vfs/reader.go，写请求会调用 pkg/vfs/writer.go。
pkg/meta：所有元数据引擎的实现。
pkg/meta/interface.go：所有类型元数据引擎的接口定义。
pkg/meta/redis.go：Redis 数据库的接口实现。
pkg/meta/sql.go：关系型数据库的接口定义及通用接口实现，特定数据库的实现在单独文件中（如 MySQL 的实现在 pkg/meta/sql_mysql.go）。
pkg/meta/tkv.go：KV 类数据库的接口定义及通用接口实现，特定数据库的实现在单独文件中（如 TiKV 的实现在 pkg/meta/tkv_tikv.go）。
pkg/object：与各种对象存储对接的实现。

3. docs 目录
功能：包含项目的文档，如快速开始指南、命令参考等。
4. scripts 目录
功能：包含构建和测试脚本。
5. 其他文件
Dockerfile：用于构建 Docker 镜像。
Makefile：包含项目的构建和测试命令。
go.mod 和 go.sum：Go 模块依赖文件。

总结
JuiceFS 的代码结构清晰，主要分为 cmd、pkg、docs 和 scripts 等目录。
cmd 目录包含命令行工具和挂载实现，pkg 目录包含核心逻辑和具体实现，docs 目录包含项目文档，scripts 目录包含构建和测试脚本。
这种结构有助于模块化开发和维护。
