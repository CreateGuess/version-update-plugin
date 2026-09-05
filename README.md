# Version Update Plugin

该插件把云端 `version-update-service` 的版本查询与软件下载能力接入本机
`rpc_gateway`，并负责 `codeit-deploy` 和 `frontend` 两类软件包的下载进度、
取消、SHA-256 校验、解压、本地版本枚举和版本切换。
`version-update-service` 无需修改。

## 项目结构

```text
version-update-plugin/
├── config/version-update-plugin.json
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

插件根据运行机器架构选择默认配置路径，不使用运行进程的 `$HOME`：

- x86：`/home/codeit/Desktop/version-update-plugin.json`
- ARM：`/home/pi/Desktop/version-update-plugin.json`

`CODEIT_UPDATE_CONFIG` 非空时优先使用该路径。x86 安装示例配置：

```bash
cp config/version-update-plugin.json /home/codeit/Desktop/version-update-plugin.json
vim /home/codeit/Desktop/version-update-plugin.json
```

ARM 设备将上述目标路径改成 `/home/pi/Desktop/version-update-plugin.json`。

配置格式：

```json
{
  "server_url": "http://192.168.2.100:28000",
  "packages": {
    "codeit-deploy": {
      "type": "codeit-deploy",
      "label": "CODEIT",
      "install_path": "/home/codeit/Desktop/codeit-deploy_x86_64"
    },
    "frontend": {
      "type": "frontend",
      "label": "FRONTEND",
      "install_path": "/home/codeit/Desktop/frontend/robot-platform",
      "name": "robot-platform"
    }
  }
}
```

`packages` 是动态软件包表，不限制为上面的两项。对象键是本机唯一的包 ID；新增组件
只需增加配置，不需要修改或重新编译插件。例如增加 `rpc_gateway`：

```json
"rpc-gateway": {
  "type": "backend",
  "name": "rpc_gateway",
  "label": "RPC GATEWAY",
  "install_path": "/home/codeit/Desktop/backend/rpc_gateway"
}
```

`type` 支持 `codeit-deploy`、`codeit-lib`、`backend`、`frontend`。其中
`backend`、`frontend` 必须配置 `name`；`codeit-deploy`、`codeit-lib` 不能配置
`name`。`label` 只用于前端显示。前端通过 `GET /packages` 动态生成软件包选择器。

如需使用其他配置文件，可设置：

```bash
export CODEIT_UPDATE_CONFIG=/path/to/version-update-plugin.json
```

`install_path` 是本机版本安装根目录。插件不会再根据运行用户或 `$HOME` 推测安装
位置，因此即使 `rpc_gateway` 由 root 启动也不会错误使用 `/root/Desktop`。
`server_url` 推荐只填写协议、主机和端口，例如 `http://192.168.2.100:28000`。
为兼容页面中保存的历史配置，末尾带 `/api`、`/api/version` 或 `/api/package` 也会
自动转换成服务基础地址。
插件启动时自动读取 `uname`、`/proc/device-tree/compatible`、
`/proc/device-tree/model` 和 `/etc/os-release`，获得 `arch`、`platform`、`os`。
这些系统查询维度不会从配置文件或前端接收，避免人为选择错误的软件包。

ARM 设备只需要修改本机安装路径：

```text
codeit-deploy.install_path = /home/pi/Desktop/codeit-deploy_aarch64
frontend.install_path      = /home/pi/Desktop/frontend/robot-platform
```

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
└── v2.2.0/
```

下载中的文件位于对应软件包根目录的
`.downloads/<版本号>.zip.part`。网络中断时插件会自动重试并通过 HTTP Range 从已有
字节继续下载；达到重试上限或主动取消后仍会保留该文件，下一次创建相同版本任务时
继续续传。下载成功并完成校验、解压后自动删除；SHA-256 不匹配时删除异常文件并在
下次任务中重新下载。解压阶段的中间目录位于根目录下的 `.staging/<版本号>`。

如果已有 `current_version` 是软链接，插件会继续以软链接方式原子切换；如果是
普通文件，则写入 `v1.3.50` 这样的版本文本并原子替换。

## 前端接口

### 接口地址与调用约定

本机 HTTP 接口前缀：

```text
http://127.0.0.1/backend/plugin-http/version_update
```

本机 WebSocket 地址：

```text
ws://127.0.0.1/backend/plugin-ws/version_update
```

如果 `rpc_gateway` 监听了其他 IP 或端口，只替换上面地址中的主机和端口，路径保持
不变。例如网关监听 `8080` 时，接口前缀为
`http://127.0.0.1:8080/backend/plugin-http/version_update`。

接口调用约定如下：

- GET 参数放在 URL 查询字符串中，例如
  `/versions?package=frontend`。多个参数使用 `&` 连接，参数值应使用 URL 编码。
- POST 参数放在 JSON 请求体中，并设置
  `Content-Type: application/json`，不要把 POST 参数拼到 URL 上。
- `package` 是配置文件 `packages` 对象中的键，也是前端应使用的唯一软件包 ID。
  例如 `codeit-deploy`、`frontend`、`codeit-lib` 或以后新增的任意键。
- `type` 是云端软件包分类，例如 `frontend` 或 `backend`。接口仅为旧版前端兼容
  `type` 参数；多个本地软件包可能具有相同 `type`，新前端应始终传 `package`。
- `channel` 只能为 `test` 或 `release`，由前端选择；默认值为 `test`。
- `server_url` 为可选的临时云端地址。省略时使用环境变量
  `CODEIT_UPDATE_SERVER`，若环境变量未设置则使用配置文件中的 `server_url`。
- 前端不传 `arch`、`platform` 和 `os`。这些字段由插件读取本机系统信息后，根据
  软件包类型自动补充。

成功响应采用以下格式，具体数据位于 `data`：

```json
{
  "code": 0,
  "message": "ok",
  "data": {}
}
```

失败响应采用以下格式。前端应同时判断 HTTP 状态码和 `code`，并把 `message`
展示给用户：

```json
{
  "code": -1,
  "error": "invalid_package",
  "message": "软件包不存在"
}
```

### 接口总览

| 方法 | 路径 | 参数位置 | 用途 |
| --- | --- | --- | --- |
| GET | `/packages` | 无 | 获取配置中声明的全部软件包 |
| GET | `/status` | URL 查询参数 | 获取任务状态和软件包当前版本 |
| GET | `/versions` | URL 查询参数 | 获取指定软件包的本地版本列表 |
| POST | `/remote` | JSON 请求体 | 查询指定软件包的云端版本清单 |
| POST | `/download` | JSON 请求体 | 创建异步下载任务 |
| POST | `/cancel` | 无，或空 JSON 对象 | 取消当前任务 |
| POST | `/switch` | JSON 请求体 | 切换到已经下载的本地版本 |

以下示例中的 `BASE_URL` 表示
`http://127.0.0.1/backend/plugin-http/version_update`。

### GET /packages：获取软件包列表

该接口没有查询参数。前端启动后应先调用此接口，使用返回的 `id` 和 `label` 动态
生成软件包选择器，不要把 `codeit-deploy`、`frontend` 等选项写死在页面中。

```bash
curl 'http://127.0.0.1/backend/plugin-http/version_update/packages'
```

响应示例：

```json
{
  "code": 0,
  "message": "ok",
  "data": [
    {
      "id": "codeit-deploy",
      "type": "codeit-deploy",
      "name": "",
      "label": "CODEIT",
      "install_path": "/home/codeit/Desktop/codeit-deploy_x86_64",
      "active_version": "v1.3.50"
    },
    {
      "id": "frontend",
      "type": "frontend",
      "name": "robot-platform",
      "label": "FRONTEND",
      "install_path": "/home/codeit/Desktop/frontend/robot-platform",
      "active_version": "v2.2.0"
    }
  ]
}
```

前端后续请求应把用户所选条目的 `id` 作为 `package`，把 `label` 用作显示名称。

### GET /status：获取当前状态

查询参数：

| 参数 | 必填 | 类型 | 说明 |
| --- | --- | --- | --- |
| `package` | 否 | string | 配置中的软件包 ID，推荐传入 |
| `type` | 否 | string | 旧版兼容参数；仅在未传 `package` 时使用 |

不传参数时优先返回 `codeit-deploy` 的当前版本；如果配置中没有该 ID，则选择配置中
的第一项。推荐前端始终明确传 `package`。

```bash
curl 'http://127.0.0.1/backend/plugin-http/version_update/status?package=frontend'
```

响应中的任务状态是全局状态，同时包含所有配置软件包的当前版本：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "type": "status",
    "state": "downloading",
    "message": "正在下载 v2.2.0",
    "target_type": "frontend",
    "target_package": "frontend",
    "target_version": "v2.2.0",
    "downloaded_bytes": 5242880,
    "total_bytes": 10485760,
    "progress": 50.0,
    "selected_package": "frontend",
    "selected_type": "frontend",
    "active_version": "v2.1.0",
    "deploy_root": "/home/codeit/Desktop/frontend/robot-platform",
    "packages": {
      "frontend": {
        "type": "frontend",
        "name": "robot-platform",
        "label": "FRONTEND",
        "active_version": "v2.1.0",
        "deploy_root": "/home/codeit/Desktop/frontend/robot-platform"
      }
    },
    "system": {
      "arch": "x86_64",
      "platform": "generic",
      "os": "ubuntu-22.04"
    }
  }
}
```

`state` 可能为：

| 状态 | 含义 |
| --- | --- |
| `idle` | 当前没有任务 |
| `queued` | 任务已经进入队列 |
| `checking` | 正在查询并确认云端软件包 |
| `downloading` | 正在下载或断点续传 |
| `verifying` | 正在校验文件大小和 SHA-256 |
| `extracting` | 正在解压到临时目录 |
| `activating` | 正在切换 `current_version` |
| `completed` | 下载流程成功完成 |
| `failed` | 下载流程失败，原因见 `message` |
| `canceled` | 用户取消了任务 |

正常运行时应使用 WebSocket 接收状态；该 GET 接口适合页面初始化、WebSocket 重连后
补快照以及人工诊断。

### GET /versions：获取本地版本

查询参数与 `/status` 相同：优先使用 `package`，`type` 只用于兼容旧调用。

```bash
curl 'http://127.0.0.1/backend/plugin-http/version_update/versions?package=codeit-deploy'
```

响应示例：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "package": "codeit-deploy",
    "type": "codeit-deploy",
    "deploy_root": "/home/codeit/Desktop/codeit-deploy_x86_64",
    "active_version": "v1.3.50",
    "versions": [
      {"version": "v1.3.50", "active": true, "size": 152345678},
      {"version": "v1.3.46", "active": false, "size": 149876543}
    ]
  }
}
```

`size` 是解压后整个版本目录的字节数，不是云端 zip 文件大小。版本按数字版本号从高
到低排列。

### POST /remote：查询云端版本

JSON 请求体字段：

| 字段 | 必填 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| `package` | 否 | string | 默认软件包 | 配置中的软件包 ID，推荐传入 |
| `type` | 否 | string | `codeit-deploy` | 旧版兼容参数，不建议新前端使用 |
| `channel` | 否 | string | `test` | 只能是 `test` 或 `release` |
| `server_url` | 否 | string | 本机配置值 | 临时覆盖本次请求使用的云端服务根地址 |

```bash
curl -X POST 'http://127.0.0.1/backend/plugin-http/version_update/remote' \
  -H 'Content-Type: application/json' \
  -d '{"package":"frontend","channel":"release"}'
```

插件会在内部向云端发送 `POST /api/version`。前端收到的 `data` 是云端版本清单，
示例如下：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "generated_at": "2026-09-03T03:54:41Z",
    "latest_version": "v2.2.0",
    "packages": [
      {
        "version": "v2.2.0",
        "filename": "v2.2.0.zip",
        "size": 10485760,
        "sha256": "...",
        "is_latest": true,
        "download": {
          "method": "POST",
          "url": "/api/package",
          "body": {
            "type": "frontend",
            "name": "robot-platform",
            "channel": "release",
            "filename": "v2.2.0.zip"
          }
        }
      }
    ],
    "query": {
      "type": "frontend",
      "name": "robot-platform",
      "channel": "release"
    }
  }
}
```

不同类型发送给云端的字段由插件自动生成：

| 配置中的 `type` | 插件发送给云端 `/api/version` 的字段 |
| --- | --- |
| `codeit-deploy` | `type`、`arch`、`platform`、`os`、`channel` |
| `codeit-lib` | `type`、`arch`、`platform`、`os`、`channel` |
| `backend` | `type`、配置中的 `name`、`arch`、`channel` |
| `frontend` | `type`、配置中的 `name`、`channel` |

因此浏览器只需提交 `package` 和 `channel`，不能让用户选择或覆盖架构、平台和系统。

### POST /download：创建下载任务

JSON 请求体字段：

| 字段 | 必填 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| `package` | 否 | string | 默认软件包 | 配置中的软件包 ID，推荐传入 |
| `type` | 否 | string | `codeit-deploy` | 旧版兼容参数，不建议新前端使用 |
| `version` | 是 | string | 无 | `/remote` 返回的目标版本 |
| `channel` | 否 | string | `test` | 必须和查询该版本时使用的通道一致 |
| `activate` | 否 | boolean | `false` | 下载、校验、解压成功后是否立即切换 |
| `server_url` | 否 | string | 本机配置值 | 临时覆盖本次任务的云端服务根地址 |

```bash
curl -X POST 'http://127.0.0.1/backend/plugin-http/version_update/download' \
  -H 'Content-Type: application/json' \
  -d '{"package":"frontend","version":"v2.2.0","channel":"release","activate":true}'
```

任务成功创建时返回 HTTP `202 Accepted`。这只表示任务已经进入队列，不表示文件已经
下载完成：

```json
{
  "code": 0,
  "message": "下载任务已创建",
  "data": {
    "package": "frontend",
    "type": "frontend",
    "version": "v2.2.0",
    "state": "queued"
  }
}
```

前端收到 202 后应通过 WebSocket 等待 `completed`、`failed` 或 `canceled`，不要连续
轮询 `/status`。`activate=false` 时完成下载和解压但不切换；之后可调用 `/switch`。

如果网络中断，插件会自动重试并从 `.zip.part` 文件断点续传。任务最终失败后，用户
可以再次提交相同的 `/download` 请求继续下载。

### POST /cancel：取消当前任务

该接口没有业务参数，请求体可以省略，也可以传空 JSON 对象：

```bash
curl -X POST 'http://127.0.0.1/backend/plugin-http/version_update/cancel' \
  -H 'Content-Type: application/json' \
  -d '{}'
```

取消请求提交成功时返回 HTTP `202 Accepted`：

```json
{
  "code": 0,
  "message": "取消请求已提交"
}
```

取消是协作式异步操作。收到 202 后仍应等待 WebSocket 状态变为 `canceled`。当前没有
可取消任务时返回 HTTP 409 和 `no_active_download`。

### POST /switch：切换本地版本

该接口只切换已经存在于安装目录中的版本，不会触发下载。

| 字段 | 必填 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| `package` | 否 | string | 默认软件包 | 配置中的软件包 ID，推荐传入 |
| `type` | 否 | string | 无 | 旧版兼容参数，不建议新前端使用 |
| `version` | 是 | string | 无 | `/versions` 返回的本地版本号 |

```bash
curl -X POST 'http://127.0.0.1/backend/plugin-http/version_update/switch' \
  -H 'Content-Type: application/json' \
  -d '{"package":"codeit-deploy","version":"v1.3.46"}'
```

响应示例：

```json
{
  "code": 0,
  "message": "版本切换成功",
  "data": {
    "package": "codeit-deploy",
    "active_version": "v1.3.46"
  }
}
```

下载、校验、解压或自动激活期间不能手工切换，冲突时返回 HTTP 409 和
`download_busy`。

### WebSocket 状态推送

浏览器建立连接后，插件会立即发送一次完整状态快照。任务阶段变化时立即推送；下载
过程中最多每 200ms 推送一次最新进度。客户端发送任意文本消息时，插件也会立即返回
一份最新快照；二进制消息会被忽略。

```javascript
const ws = new WebSocket('ws://127.0.0.1/backend/plugin-ws/version_update');

ws.addEventListener('open', () => {
  // 连接成功时服务端本身会推送一次；也可以主动请求最新快照。
  ws.send('snapshot');
});

ws.addEventListener('message', (event) => {
  const status = JSON.parse(event.data);
  console.log(status.state, status.progress, status.message);

  if (status.state === 'completed') {
    // 重新调用 GET /versions，刷新本地版本列表。
  }
});

ws.addEventListener('close', () => {
  // 实际前端应延迟后重连，并在重连后刷新 /packages 和 /versions。
});
```

WebSocket 消息不是 HTTP 响应，因此外层没有 `code` 和 `data`，收到的内容就是状态
对象：

```json
{
  "type": "status",
  "state": "downloading",
  "message": "正在下载 v1.3.51",
  "target_type": "codeit-deploy",
  "target_package": "codeit-deploy",
  "target_version": "v1.3.51",
  "downloaded_bytes": 7340032,
  "total_bytes": 14680064,
  "progress": 50.0,
  "packages": {},
  "system": {
    "arch": "x86_64",
    "platform": "generic",
    "os": "ubuntu-22.04"
  }
}
```

### 常见错误码

| HTTP 状态 | `error` | 含义 |
| --- | --- | --- |
| 400 | `invalid_json` | POST 请求体不是合法 JSON |
| 400 | `invalid_request` | POST 请求体的顶层不是 JSON 对象 |
| 400 | `invalid_parameters` | 软件包配置、参数类型或请求参数不完整 |
| 400 | `invalid_package` | 无法从当前配置中解析软件包 |
| 400 | `invalid_channel` | `channel` 不是 `test` 或 `release` |
| 400 | `invalid_version` | 版本号格式不受支持 |
| 400 | `invalid_server_url` | `server_url` 不是合法 HTTP/HTTPS 根地址 |
| 404 | `not_found` | 插件子路由不存在 |
| 409 | `version_exists` | 本地已经存在该版本，可直接切换 |
| 409 | `download_busy` | 已有任务执行，不能新建任务或切换版本 |
| 409 | `no_active_download` | 没有可以取消的任务 |
| 409 | `switch_failed` | 本地版本不存在或更新 `current_version` 失败 |
| 413 | `body_too_large` | 请求体超过 Host 允许的内存限制 |
| 502 | `remote_error` | 请求云端版本服务失败或云端返回错误 |
| 503 | `request_canceled` | HTTP 请求被 Host 取消 |
| 500 | `internal_error` | 插件内部发生未预期错误 |

### 云端接口与本机接口的关系

前端只调用本机插件接口，不需要直接调用云端服务。插件已经适配新版云端协议：

1. `/remote` 在内部调用云端 JSON 接口 `POST /api/version`。
2. `/download` 先查询清单，再按照清单中的 `download.method`、`download.url` 和
   `download.body` 调用云端下载接口，当前服务返回的是 `POST /api/package`。
3. 云端返回的 `download.body.filename` 会原样用于下载请求，不由前端拼接文件路径。

云端接口定义以
[`version-update-service`](https://github.com/CreateGuess/version-update-service) 为准。

### codeit-lib 文件名兼容

`codeit-lib` 支持 `v1.3.48.zip`、`codeit-1.3.48.zip`、
`codeit-v1.3.48.zip`。云端 `version` 可以是完整文件名；插件使用不带 `.zip` 的本地
目录名，例如 `/usr/codeit/codeit-1.3.48`，断点文件为
`/usr/codeit/.downloads/codeit-1.3.48.zip.part`。

下载请求仍使用云端返回的原始 `download.body.filename`，不会改写文件名。本地版本
按三段数字排序，版本切换支持以上目录名。

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
ws://127.0.0.1/backend/plugin-ws/version_update
```

如果 `rpc_gateway` 使用其他地址或端口，请在页面的“本机插件 API”中修改。
页面和插件接口端口不同时，需要确保网关 CORS 策略允许
`http://127.0.0.1:8000`。
