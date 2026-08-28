# Version Update Plugin

该插件把云端 `version-update-service` 的版本查询与软件下载能力接入本机
`rpc_gateway`，并负责 `codeit-deploy` 和 `frontend` 两类软件包的下载进度、
取消、SHA-256 校验、解压、本地版本枚举和版本切换。
`version-update-service` 无需修改。

## 项目结构

```text
version-update-plugin/
├── include/version_update_plugin.hpp
├── source/version_update_plugin.cpp
├── web/                         Web 版本控制台
├── CMakeLists.txt
└── README.md                    插件说明
```

## 构建插件

安装构建及运行依赖：

```bash
sudo apt install build-essential cmake libarchive-dev libcurl4-openssl-dev libssl-dev
```

构建插件；`xplugin-dev` 必须与运行中的 `rpc_gateway` Host 版本匹配：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DRPC_GATEWAY_PLUGIN_OUTPUT_DIR=/path/to/rpc_gateway/plugins

cmake --build build -j
```

### 下载实现与 HTTPS

云端版本查询和软件包流式下载均使用系统 `libcurl`，不再依赖
`cpp-httplib`。HTTP 和 HTTPS 都由 libcurl 处理；HTTPS 默认校验证书和主机名。
SHA-256 校验使用 `OpenSSL::Crypto`，不要求 OpenSSL 3，在 OpenSSL 1.1.x
环境中也可以构建。

## 运行配置

```bash
# 云端 version-update-service 的 Nginx 地址
export CODEIT_UPDATE_SERVER=http://192.168.2.100:28000

# 可选；默认是 $HOME/Desktop/codeit-deploy_<本机架构>
export CODEIT_DEPLOY_ROOT=/home/codeit/Desktop/codeit-deploy_x86_64

# 可选；默认是 $HOME/Desktop/frontend/robot-platform
export CODEIT_FRONTEND_ROOT=/home/codeit/Desktop/frontend/robot-platform
```

默认部署目录按设备用户和架构确定：

```text
x86_64 机器（用户 codeit）：
  codeit-deploy -> /home/codeit/Desktop/codeit-deploy_x86_64
  frontend      -> /home/codeit/Desktop/frontend/robot-platform

ARM 板卡（用户 pi）：
  codeit-deploy -> /home/pi/Desktop/codeit-deploy_aarch64
  frontend      -> /home/pi/Desktop/frontend/robot-platform
```

插件分别优先使用 `CODEIT_DEPLOY_ROOT` 和 `CODEIT_FRONTEND_ROOT`。未配置时，
非 root 用户使用 `$HOME/Desktop/...`。如果运行环境没有 `HOME`，或者
`rpc_gateway` 以 root 运行且 `HOME=/root`，则 x86 回退到 `/home/codeit`，
ARM 回退到 `/home/pi`，不会使用 `/root/Desktop`。

ZIP 在插件进程内通过 `libarchive` 安全解压，不再启动 `unzip` 子进程，因而不受
Host 的 `SIGCHLD`/子进程回收策略影响。构建环境需要 libarchive、libcurl、
OpenSSL 开发包和与 Host 匹配的 `xplugin-dev`。

两类软件包使用相同的版本目录结构：

```text
codeit-deploy_x86_64/
├── current_version
├── .downloads/                 未完成下载的临时目录
├── v1.3.46/
└── v1.3.50/

frontend/robot-platform/
├── current_version
├── .downloads/
├── v1.3.46/
└── v1.3.50/
```

下载中的文件位于对应软件包根目录的
`.downloads/<版本号>.zip.part`。网络中断时插件会自动重试并通过 HTTP Range 从已有
字节继续下载；达到重试上限或主动取消后仍会保留该文件，下一次创建相同版本任务时
继续续传。下载成功并完成校验、解压后自动删除；SHA-256 不匹配时删除异常文件并在
下次任务中重新下载。解压阶段的中间目录位于根目录下的 `.staging/<版本号>`。

如果已有 `current_version` 是软链接，插件会继续以软链接方式原子切换；如果是
普通文件，则写入 `v1.3.50` 这样的版本文本并原子替换。

## 前端接口

接口前缀：`/backend/plugin-http/version_update`

```text
GET  /status       当前任务状态与指定类型的当前版本
GET  /versions     指定类型的本地版本列表及当前版本
POST /remote       查询云端最近两个版本
POST /download     创建异步下载任务
POST /cancel       取消当前下载
POST /switch       切换到已下载版本
```

查询云端版本：

```bash
curl -X POST http://127.0.0.1/backend/plugin-http/version_update/remote \
  -H 'Content-Type: application/json' \
  -d '{"type":"codeit-deploy","channel":"release","arch":"x86_64"}'
```

创建下载任务；`activate=true` 表示校验和解压成功后立即切换：

```bash
curl -X POST http://127.0.0.1/backend/plugin-http/version_update/download \
  -H 'Content-Type: application/json' \
  -d '{"type":"frontend","version":"v1.3.50","channel":"release","activate":true}'
```

前端可轮询进度：

```bash
curl 'http://127.0.0.1/backend/plugin-http/version_update/status?type=frontend'
```

`state` 依次可能为 `idle`、`queued`、`checking`、`downloading`、
`verifying`、`extracting`、`activating`、`completed`、`failed`、`canceled`。

单独切换已有版本：

```bash
curl -X POST http://127.0.0.1/backend/plugin-http/version_update/switch \
  -H 'Content-Type: application/json' \
  -d '{"type":"codeit-deploy","version":"v1.3.46"}'
```

`POST /remote`、`POST /download` 和 `POST /switch` 的 `type` 可取
`codeit-deploy` 或 `frontend`，缺省为 `codeit-deploy`。查询本地版本时使用：

```bash
curl 'http://127.0.0.1/backend/plugin-http/version_update/versions?type=codeit-deploy'
```

访问云端的请求也可以通过 JSON 中的 `server_url` 临时覆盖云端地址。

## Web 控制台

插件目录中的 `web/` 提供了对应的前端控制台。在
`version-update-plugin` 目录执行：

```bash
cd web
python3 -m http.server 8000
```

访问 `http://127.0.0.1:8000`，在页面中选择 `codeit-deploy` 或 `frontend`，
并配置本机插件 API 和云端服务地址即可控制完整下载与版本切换流程。

页面默认访问：

```text
http://127.0.0.1/backend/plugin-http/version_update
```

如果 `rpc_gateway` 使用其他地址或端口，请在页面的“本机插件 API”中修改。
页面和插件接口端口不同时，需要确保网关 CORS 策略允许
`http://127.0.0.1:8000`。
