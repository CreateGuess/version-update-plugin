#include "version_update_plugin.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/utsname.h>
#endif

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr char kEndpoint[] = "version_update";

/**
 * @brief 删除 URL 末尾多余的斜杠
 * @param value 待处理的 URL
 * @return 规范化后的 URL
 */
std::string trim_trailing_slashes(std::string value) {
  while (value.size() > 8 && value.back() == '/') value.pop_back();
  return value;
}

/**
 * @brief 将云端地址规范化为不包含固定 API 路径的服务基础地址
 * @param value 用户或配置文件提供的云端地址
 * @return 可与 /api/version、/api/package 拼接的基础地址
 */
std::string normalize_server_url(std::string value) {
  // 1. 清除末尾斜杠
  value = trim_trailing_slashes(std::move(value));

  // 2. 兼容页面历史配置中已经包含固定接口路径的地址
  for (const std::string suffix : {"/api/version", "/api/package", "/api"}) {
    if (value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
      value.erase(value.size() - suffix.size());
      break;
    }
  }
  return trim_trailing_slashes(std::move(value));
}

/**
 * @brief 读取文本文件并删除首尾空白字符
 * @param path 文本文件路径
 * @return 文件内容，读取失败时返回空字符串
 */
std::string read_text_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  std::ostringstream output;
  output << input.rdbuf();
  std::string value = output.str();
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(0, 1);
  return value;
}

/**
 * @brief 将字符串转换为小写
 * @param value 待转换字符串
 * @return 小写字符串
 */
std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

/**
 * @brief 删除 os-release 字段可能包含的单引号或双引号
 * @param value os-release 字段值
 * @return 不带外层引号的字段值
 */
std::string unquote(std::string value) {
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

/**
 * @brief 检查系统识别值能否安全作为云端目录片段
 * @param value 待检查的架构、平台或系统标识
 * @return 与云端服务路径片段规则一致时返回 true
 */
bool is_safe_path_component(const std::string& value) {
  static const std::regex pattern(R"(^[A-Za-z0-9][A-Za-z0-9._-]*$)");
  return std::regex_match(value, pattern);
}

/**
 * @brief 将版本号转换为可比较的三段数字
 * @param version 版本号，如 v1.3.50
 * @return 主版本号、次版本号和修订号
 */
std::array<unsigned long long, 3> version_parts(const std::string& version) {
  std::array<unsigned long long, 3> result{};
  std::sscanf(version.c_str(), "v%llu.%llu.%llu", &result[0], &result[1], &result[2]);
  return result;
}
}  // namespace

/**
 * @brief 初始化插件
 * @return 初始化成功返回 true
 */
bool VersionUpdatePlugin::on_init(IPluginContext&) noexcept {
  // 1. 构造并设置插件全局日志
  LOGX_GLOBAL_CLASS(PLUGIN_LOG_NAME)::Construct();
  G_LOG()->set(logger());

  // 2. 从桌面配置文件读取云端地址和本机安装目录
  std::string config_error;
  if (!load_config(config_error)) {
    LOG_ERROR("[{}] Failed to load config: {}", TAG, config_error);
    LOGX_GLOBAL_CLASS(PLUGIN_LOG_NAME)::Destruct();
    return false;
  }

  // 3. 从 Linux 系统配置自动识别云端查询维度
  std::string system_error;
  if (!detect_system(system_error)) {
    LOG_ERROR("[{}] Failed to detect system: {}", TAG, system_error);
    LOGX_GLOBAL_CLASS(PLUGIN_LOG_NAME)::Destruct();
    return false;
  }

  // 4. 初始化 libcurl 全局环境
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    LOG_ERROR("[{}] curl_global_init failed", TAG);
    LOGX_GLOBAL_CLASS(PLUGIN_LOG_NAME)::Destruct();
    return false;
  }

  // 5. 输出自动识别结果和两类软件包的本机版本目录
  LOG_INFO(
      "[{}] Initialized; config: {}, system: {}/{}/{}, codeit-deploy root: {}, frontend root: {}",
      TAG, config_path_.string(), system_arch_, system_platform_, system_os_,
      package_root("codeit-deploy").string(), package_root("frontend").string());
  return true;
}

/**
 * @brief 启动插件 HTTP 接口和下载工作线程
 * @return 启动成功返回 true
 */
bool VersionUpdatePlugin::on_start() noexcept {
  // 1. 避免重复启动插件
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) return true;

  // 2. 声明前端可访问的 HTTP 路由和资源限制
  PluginHttpEndpointOptions options;
  options.methods = {"GET", "POST"};
  options.routes = {
      PluginHttpRoute{"/status", {"GET"}}, PluginHttpRoute{"/versions", {"GET"}},
      PluginHttpRoute{"/remote", {"POST"}}, PluginHttpRoute{"/download", {"POST"}},
      PluginHttpRoute{"/cancel", {"POST"}}, PluginHttpRoute{"/switch", {"POST"}},
  };
  options.max_body_size = 64 * 1024;
  options.max_memory_body_size = 64 * 1024;
  options.timeout_ms = 15000;
  options.max_concurrency = 8;
  options.requests_per_second = 30;
  options.burst_requests = 60;

  // 3. 向 Host 注册版本管理 HTTP 端点
  if (!register_http_endpoint(
          kEndpoint, [this](const PluginHttpRequest& request) { return handle_http(request); },
          std::move(options))) {
    LOG_ERROR("[{}] Failed to register HTTP endpoint", TAG);
    started_.store(false);
    return false;
  }

  // 4. 注册状态推送 WebSocket 端点
  PluginWsEndpointOptions ws_options;
  ws_options.max_receive_message_size = 16 * 1024;
  ws_options.max_send_message_size = 256 * 1024;
  ws_options.max_sessions = 32;
  ws_options.max_send_queue_messages = 16;
  ws_options.max_send_queue_bytes = 1024 * 1024;
  if (!register_ws_endpoint(
          kEndpoint,
          [this](const char* session_id, const void* data, std::size_t size,
                 PluginWsMessageType type) { handle_ws_message(session_id, data, size, type); },
          [this](const char* session_id) { handle_ws_open(session_id); }, {}, ws_options)) {
    LOG_ERROR("[{}] Failed to register WebSocket endpoint", TAG);
    unregister_http_endpoint(kEndpoint);
    started_.store(false);
    return false;
  }

  // 5. 启动由 Host 管理的下载工作线程
  cancel_requested_.store(false);
  if (!start_managed_worker("version-update-download",
                            [this](PluginStopToken stop) { worker_loop(stop); })) {
    LOG_ERROR("[{}] Failed to start download worker", TAG);
    unregister_ws_endpoint(kEndpoint);
    unregister_http_endpoint(kEndpoint);
    started_.store(false);
    return false;
  }

  // 6. 插件启动完成
  LOG_INFO("[{}] Started: HTTP /backend/plugin-http/{}/..., WS /backend/plugin-ws/{}", TAG,
           kEndpoint, kEndpoint);
  return true;
}

/**
 * @brief 停止插件
 */
void VersionUpdatePlugin::on_stop() noexcept {
  // 1. 请求取消当前任务并唤醒等待中的工作线程
  cancel_requested_.store(true);
  job_cv_.notify_all();

  // 2. 更新插件启动状态
  started_.store(false);
  LOG_INFO("[{}] Stopped", TAG);
}

/**
 * @brief 卸载插件并释放日志资源
 */
void VersionUpdatePlugin::on_unload() noexcept {
  LOG_INFO("[{}] Unloaded", TAG);
  curl_global_cleanup();
  LOGX_GLOBAL_CLASS(PLUGIN_LOG_NAME)::Destruct();
}

/**
 * @brief 统一处理插件 HTTP 请求
 * @param request Host 转发的 HTTP 请求
 * @return 插件 HTTP 响应
 */
PluginHttpResponse VersionUpdatePlugin::handle_http(const PluginHttpRequest& request) {
  try {
    // 1. 检查请求是否取消或请求体是否被转存到文件
    if (request.stop_token.stop_requested())
      return json_error(503, "request_canceled", "请求已取消");
    if (request.body_spooled_to_file())
      return json_error(413, "body_too_large", "请求体过大");

    // 2. 处理不需要请求体的查询接口
    if (request.route_pattern == "/status") return handle_status(request);
    if (request.route_pattern == "/versions") return handle_local_versions(request);

    // 3. 解析 JSON 请求体并分发写操作接口
    const json body = parse_body(request);
    if (request.route_pattern == "/remote") return handle_remote_versions(body);
    if (request.route_pattern == "/download") return handle_download(body);
    if (request.route_pattern == "/cancel") return handle_cancel();
    if (request.route_pattern == "/switch") return handle_switch(body);
    return json_error(404, "not_found", "接口不存在");
  } catch (const json::parse_error& error) {
    // 4. 将可预期的请求错误转换为 400 响应
    return json_error(400, "invalid_json", error.what());
  } catch (const std::invalid_argument& error) {
    return json_error(400, "invalid_request", error.what());
  } catch (const std::exception& error) {
    LOG_ERROR("[{}] HTTP request failed: {}", TAG, error.what());
    return json_error(500, "internal_error", error.what());
  } catch (...) {
    return json_error(500, "internal_error", "未知错误");
  }
}

/**
 * @brief 获取当前下载任务状态
 * @param request Host 转发的 HTTP 请求
 * @return 包含任务阶段、进度和当前版本的 HTTP 响应
 */
PluginHttpResponse VersionUpdatePlugin::handle_status(const PluginHttpRequest& request) {
  // 1. 读取前端当前选择的软件包类型
  std::string selected_type(request.param("type"));
  if (selected_type.empty()) selected_type = "codeit-deploy";
  if (!valid_type(selected_type))
    return json_error(400, "invalid_type", "type 必须是 codeit-deploy 或 frontend");

  // 2. 构建与 WebSocket 推送一致的状态快照
  json result = status_snapshot_json();

  // 3. 补充实时读取的当前版本和部署目录
  result["selected_type"] = selected_type;
  result["active_version"] = result["packages"][selected_type]["active_version"];
  result["deploy_root"] = result["packages"][selected_type]["deploy_root"];
  return json_response(200, {{"code", 0}, {"message", "ok"}, {"data", std::move(result)}});
}

/**
 * @brief 处理 WebSocket 客户端消息
 * @param session_id WebSocket 会话 ID
 * @param data 客户端消息数据
 * @param size 客户端消息字节数
 * @param type 客户端消息类型
 */
void VersionUpdatePlugin::handle_ws_message(const char* session_id, const void*, std::size_t,
                                            PluginWsMessageType type) {
  // 文本消息视为状态快照请求；二进制消息不处理。
  if (type != PluginWsMessageType::Text || session_id == nullptr) return;
  ws_send_latest_text(session_id, "version_update.status", status_snapshot_json().dump());
}

/**
 * @brief WebSocket 建立连接时发送完整状态快照
 * @param session_id WebSocket 会话 ID
 */
void VersionUpdatePlugin::handle_ws_open(const char* session_id) {
  if (session_id == nullptr) return;
  ws_send_latest_text(session_id, "version_update.status", status_snapshot_json().dump());
}

/**
 * @brief 获取本机已经下载的版本
 * @param request Host 转发的 HTTP 请求
 * @return 本地版本列表 HTTP 响应
 */
PluginHttpResponse VersionUpdatePlugin::handle_local_versions(const PluginHttpRequest& request) {
  // 1. 读取并校验前端当前选择的软件包类型
  std::string type(request.param("type"));
  if (type.empty()) type = "codeit-deploy";
  if (!valid_type(type))
    return json_error(400, "invalid_type", "type 必须是 codeit-deploy 或 frontend");
  // 2. 枚举对应根目录中的本地版本
  return json_response(200, {{"code", 0}, {"message", "ok"}, {"data", local_versions_json(type)}});
}

/**
 * @brief 查询云端版本清单
 * @param request 前端请求参数
 * @return 云端版本清单 HTTP 响应
 */
PluginHttpResponse VersionUpdatePlugin::handle_remote_versions(const json& request) {
  // 1. 根据前端选择和本机配置构建云端查询参数
  DownloadJob job;
  std::string error;
  if (!populate_job(request, job, error))
    return json_error(400, "invalid_parameters", error);

  // 2. 校验服务器地址和发布通道
  if (!valid_server_url(job.server_url))
    return json_error(400, "invalid_server_url", "server_url 必须是合法的 http:// 或 https:// 地址");
  if (job.channel != "test" && job.channel != "release")
    return json_error(400, "invalid_channel", "channel 必须是 test 或 release");

  // 3. 从云端读取并缓存版本清单
  (void)fetch_package(job, error);
  if (!error.empty()) return json_error(502, "remote_error", error);

  // 4. 复制缓存结果并返回前端
  json manifest;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    manifest = remote_manifest_;
  }
  return json_response(200, {{"code", 0}, {"message", "ok"}, {"data", std::move(manifest)}});
}

/**
 * @brief 创建异步下载任务
 * @param request 前端提交的版本、通道和自动切换参数
 * @return 任务创建结果 HTTP 响应
 */
PluginHttpResponse VersionUpdatePlugin::handle_download(const json& request) {
  // 1. 根据前端选择和本机配置构建下载任务
  DownloadJob job;
  std::string error;
  if (!populate_job(request, job, error))
    return json_error(400, "invalid_parameters", error);
  job.version = request.value("version", "");
  job.activate = request.value("activate", false);

  // 2. 校验版本号、服务器地址和发布通道
  if (!valid_version(job.version))
    return json_error(400, "invalid_version", "version 格式必须是 v主版本.次版本.修订号");
  if (!valid_server_url(job.server_url))
    return json_error(400, "invalid_server_url", "server_url 必须是合法的 http:// 或 https:// 地址");
  if (job.channel != "test" && job.channel != "release")
    return json_error(400, "invalid_channel", "channel 必须是 test 或 release");

  // 3. 已经存在的本地版本不重复下载
  std::error_code ec;
  if (fs::is_directory(package_root(job.type) / job.version, ec) && !ec)
    return json_error(409, "version_exists", "该版本已经下载，可直接切换");

  // 4. 在互斥锁保护下检查任务冲突并写入待执行队列
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_job_ || (state_ != "idle" && state_ != "completed" && state_ != "failed" &&
                         state_ != "canceled"))
      return json_error(409, "download_busy", "已有下载任务正在执行");
    pending_job_ = job;
    state_ = "queued";
    message_ = "任务已进入下载队列";
    target_type_ = job.type;
    target_version_ = job.version;
    downloaded_bytes_ = 0;
    total_bytes_ = 0;
    cancel_requested_.store(false);
  }

  // 5. 唤醒工作线程并立即向前端返回 202
  broadcast_status(true);
  job_cv_.notify_one();
  return json_response(202, {{"code", 0}, {"message", "下载任务已创建"},
                             {"data", {{"type", job.type}, {"version", job.version},
                                       {"state", "queued"}}}});
}

/**
 * @brief 请求取消当前下载任务
 * @return 取消请求提交结果 HTTP 响应
 */
PluginHttpResponse VersionUpdatePlugin::handle_cancel() {
  // 1. 检查当前是否存在可取消任务
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == "idle" || state_ == "completed" || state_ == "failed" || state_ == "canceled")
      return json_error(409, "no_active_download", "当前没有可取消的下载任务");
    cancel_requested_.store(true);
    message_ = "正在取消下载";
  }

  // 2. 唤醒工作线程，使排队任务及时观察取消标记
  broadcast_status(true);
  job_cv_.notify_all();
  return json_response(202, {{"code", 0}, {"message", "取消请求已提交"}});
}

/**
 * @brief 切换到指定本地版本
 * @param request 包含 version 字段的前端请求
 * @return 版本切换结果 HTTP 响应
 */
PluginHttpResponse VersionUpdatePlugin::handle_switch(const json& request) {
  // 1. 读取并校验目标版本
  const std::string type = request.value("type", "codeit-deploy");
  const std::string version = request.value("version", "");
  if (!valid_type(type))
    return json_error(400, "invalid_type", "type 必须是 codeit-deploy 或 frontend");
  if (!valid_version(version))
    return json_error(400, "invalid_version", "version 格式必须是 v主版本.次版本.修订号");

  // 2. 下载任务执行期间禁止并发切换版本
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_job_ || state_ == "checking" || state_ == "downloading" || state_ == "verifying" ||
        state_ == "extracting" || state_ == "activating")
      return json_error(409, "download_busy", "下载任务执行期间不能切换版本");
  }

  // 3. 原子更新 current_version
  std::string error;
  if (!activate_version(type, version, error)) return json_error(409, "switch_failed", error);
  broadcast_status(true);
  return json_response(200, {{"code", 0}, {"message", "版本切换成功"},
                             {"data", {{"type", type}, {"active_version", version}}}});
}

/**
 * @brief 下载工作线程主循环
 * @param stop Host 提供的协作停止令牌
 */
void VersionUpdatePlugin::worker_loop(PluginStopToken stop) {
  // 1. 持续等待任务，直到 Host 请求停止插件
  while (!stop.stop_requested()) {
    std::optional<DownloadJob> job;
    {
      // 2. 使用有界等待保证停止请求能够被及时观察
      std::unique_lock<std::mutex> lock(mutex_);
      job_cv_.wait_for(lock, std::chrono::milliseconds(100),
                       [this, &stop] { return pending_job_.has_value() || stop.stop_requested(); });
      if (stop.stop_requested()) break;
      if (!pending_job_) continue;
      job = std::move(pending_job_);
      pending_job_.reset();
    }

    // 3. 排队期间收到取消请求时不访问云端
    if (cancel_requested_.load()) {
      set_state("canceled", "下载已取消");
      continue;
    }

    // 4. 执行完整的软件包处理流程
    execute_download(*job, stop);
  }
}

/**
 * @brief 执行一次完整的版本下载流程
 * @param job 下载任务参数
 * @param stop Host 提供的协作停止令牌
 */
void VersionUpdatePlugin::execute_download(const DownloadJob& job, PluginStopToken stop) {
  // 1. 构建部署目录和临时下载文件路径
  const fs::path root = package_root(job.type);
  const fs::path temporary_file = root / ".downloads" / (job.version + ".zip.part");
  std::string error;
  try {
    // 2. 创建下载缓存目录
    std::error_code ec;
    fs::create_directories(temporary_file.parent_path(), ec);
    if (ec) throw std::runtime_error("无法创建下载目录: " + ec.message());

    // 3. 查询云端清单并定位目标软件包
    set_state("checking", "正在读取云端版本信息");
    const auto package = fetch_package(job, error);
    if (canceled(stop)) throw std::runtime_error("__canceled__");
    if (!package) throw std::runtime_error(error.empty() ? "云端没有该版本" : error);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      total_bytes_ = package->size;
    }

    // 4. 流式下载软件包并实时累计下载字节数
    set_state("downloading", "正在下载软件包");
    if (!download_package(job, *package, temporary_file, stop, error)) {
      if (canceled(stop)) throw std::runtime_error("__canceled__");
      throw std::runtime_error(error);
    }

    // 5. 校验下载文件的 SHA-256
    set_state("verifying", "正在校验 SHA-256");
    if (!verify_package(temporary_file, package->sha256, stop, error)) {
      if (canceled(stop)) throw std::runtime_error("__canceled__");
      fs::remove(temporary_file, ec);
      throw std::runtime_error(error);
    }

    // 6. 解压并原子安装目标版本目录
    set_state("extracting", "正在解压软件包");
    if (!extract_package(temporary_file, root / job.version, job.version, stop, error)) {
      if (canceled(stop)) throw std::runtime_error("__canceled__");
      throw std::runtime_error(error);
    }

    // 7. 删除临时包，并按任务参数选择是否立即切换版本
    fs::remove(temporary_file, ec);
    if (job.activate) {
      set_state("activating", "正在切换到新版本");
      if (!activate_version(job.type, job.version, error)) throw std::runtime_error(error);
    }

    // 8. 标记任务完成
    set_state("completed", job.activate ? "下载并切换完成" : "下载完成");
    LOG_INFO("[{}] Version {} downloaded successfully", TAG, job.version);
  } catch (const std::exception& exception) {
    // 9. 失败或取消时保留临时包，以便下次任务从已有字节继续下载
    if (std::string(exception.what()) == "__canceled__" || canceled(stop)) {
      set_state("canceled", "下载已取消，已保留断点文件");
      LOG_INFO("[{}] Download {} canceled", TAG, job.version);
    } else {
      set_state("failed", exception.what());
      LOG_ERROR("[{}] Download {} failed: {}", TAG, job.version, exception.what());
    }
  }
}

/**
 * @brief 查询云端版本清单并定位目标软件包
 * @param job 下载任务参数；version 为空时仅刷新版本清单
 * @param error 查询失败时写入错误信息
 * @return 目标软件包信息，仅刷新清单或查询失败时返回 std::nullopt
 */
std::optional<VersionUpdatePlugin::PackageInfo> VersionUpdatePlugin::fetch_package(
    const DownloadJob& job, std::string& error) {
  try {
    // 1. 初始化 libcurl 请求句柄
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      error = "curl_easy_init失败";
      return std::nullopt;
    }

    // 2. 构建新版 POST /api/version 请求和响应缓冲区
    const std::string url = build_url(job.server_url, "/api/version");
    const std::string query_text = package_query_json(job).dump();
    std::string body;
    char error_buffer[CURL_ERROR_SIZE]{};
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // 3. 配置 JSON POST、重定向、超时、线程安全和 TLS 校验
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query_text.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(query_text.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "codeit-version-plugin/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);

    // 4. 执行请求并读取 HTTP 状态码
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
      // 云端会用 404 表示查询维度下没有软件包，优先返回其 JSON 错误信息。
      std::string remote_code;
      std::string remote_message;
      try {
        const json remote_error = json::parse(body);
        remote_code = remote_error.value("error", "");
        remote_message = remote_error.value("message", "");
      } catch (...) {
      }
      if (!remote_message.empty()) {
        error = "云端版本查询失败: " + remote_message;
        if (!remote_code.empty()) error += " (" + remote_code + ")";
      } else {
        error = std::string("获取云端版本失败: ") +
                (error_buffer[0] ? error_buffer : curl_easy_strerror(result));
      }
      if (status != 0) error += " (HTTP " + std::to_string(status) + ")";
      error += "，请求参数: " + query_text + "，请求地址: " + url;
      return std::nullopt;
    }

    // 5. 解析并缓存云端版本清单
    json manifest = json::parse(body);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      remote_manifest_ = manifest;
    }

    // 6. version 为空表示调用方只需要刷新清单
    if (job.version.empty()) return std::nullopt;

    // 7. 检查清单中的软件包数组
    if (!manifest.contains("packages") || !manifest["packages"].is_array()) {
      error = "版本服务器响应缺少 packages";
      return std::nullopt;
    }

    // 8. 查找目标版本并校验必要字段
    for (const auto& item : manifest["packages"]) {
      if (item.value("version", "") != job.version) continue;
      const json download = item.value("download", json::object());
      const std::string download_method = download.value("method", "");
      PackageInfo package{job.version, download.value("url", ""),
                          download.value("body", json::object()), item.value("sha256", ""),
                          item.value("size", std::uint64_t{0})};
      if (download_method != "POST" || package.download_url.empty() ||
          package.download_url.front() != '/' ||
          !package.download_body.is_object() || package.sha256.size() != 64) {
        error = "版本服务器返回的软件包信息不完整";
        return std::nullopt;
      }
      return package;
    }

    // 9. 清单中没有目标版本
    error = "云端版本清单中没有 " + job.version;
    return std::nullopt;
  } catch (const std::exception& exception) {
    error = exception.what();
    return std::nullopt;
  }
}

/**
 * @brief 从云端流式下载软件包
 * @param job 下载任务参数
 * @param package 云端软件包信息
 * @param temporary_file 本机临时文件路径
 * @param stop Host 提供的协作停止令牌
 * @param error 下载失败时写入错误信息
 * @return 下载成功返回 true
 */
bool VersionUpdatePlugin::download_package(const DownloadJob& job, const PackageInfo& package,
                                           const fs::path& temporary_file, PluginStopToken stop,
                                           std::string& error) {
  // 1. 准备下载地址和断点文件名
  const std::string temporary_file_name = temporary_file.string();
  const std::string url = build_url(job.server_url, package.download_url);
  const std::string download_text = package.download_body.dump();
  constexpr int maximum_attempts = 6;

  // 2. 网络中断时在同一任务内自动重试，并从临时文件末尾续传
  for (int attempt = 1; attempt <= maximum_attempts; ++attempt) {
    if (canceled(stop)) return false;

    // 3. 读取已有断点；异常或超过清单大小时从零重新开始
    std::error_code ec;
    std::uint64_t offset = 0;
    if (fs::exists(temporary_file, ec) && !ec) {
      offset = static_cast<std::uint64_t>(fs::file_size(temporary_file, ec));
      if (ec) {
        error = "无法读取断点文件大小: " + ec.message();
        return false;
      }
    }
    if (package.size != 0 && offset > package.size) {
      fs::resize_file(temporary_file, 0, ec);
      if (ec) {
        error = "无法重置异常断点文件: " + ec.message();
        return false;
      }
      offset = 0;
    }
    if (package.size != 0 && offset == package.size) {
      std::lock_guard<std::mutex> lock(mutex_);
      downloaded_bytes_ = offset;
      total_bytes_ = package.size;
      return true;
    }

    // 4. 断点存在时以追加模式打开，否则创建新文件
    std::FILE* file = std::fopen(temporary_file_name.c_str(), offset > 0 ? "ab" : "wb");
    if (file == nullptr) {
      error = "无法打开临时下载文件";
      return false;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      std::fclose(file);
      error = "curl_easy_init失败";
      return false;
    }
    CurlDownloadContext context{this, file, stop, offset};
    char error_buffer[CURL_ERROR_SIZE]{};
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // 5. 配置新版 POST /api/package、HTTP Range、连接保活和进度回调
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, download_text.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(download_text.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "codeit-version-plugin/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    if (offset > 0)
      curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(offset));

    // 6. 执行本轮下载并确保已经写入的数据落盘
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    const bool close_ok = std::fclose(file) == 0;
    if (canceled(stop)) return false;
    if (!close_ok) {
      error = "关闭临时下载文件失败";
      return false;
    }

    // 7. 根据本地实际大小判断本轮是否已经完整下载
    std::uint64_t actual_size =
        static_cast<std::uint64_t>(fs::file_size(temporary_file, ec));
    if (ec) {
      error = "无法读取下载文件大小: " + ec.message();
      return false;
    }
    if ((package.size != 0 && actual_size == package.size) ||
        (package.size == 0 && result == CURLE_OK))
      return true;

    // 8. 服务忽略 Range 并返回完整文件时，清空错误追加的数据后重新下载
    if (offset > 0 && status == 200) {
      fs::resize_file(temporary_file, 0, ec);
      if (ec) {
        error = "服务器不支持断点续传，且无法重置临时文件: " + ec.message();
        return false;
      }
      actual_size = 0;
    }

    // 9. 记录错误；达到上限后保留断点并向调用方返回失败
    if (result != CURLE_OK) {
      error = std::string("下载失败: ") +
              (error_buffer[0] ? error_buffer : curl_easy_strerror(result));
    } else {
      error = "下载连接结束，但文件大小仍不完整";
    }
    if (status != 0) error += " (HTTP " + std::to_string(status) + ")";
    error += "，已保存 " + std::to_string(actual_size) + " 字节";
    LOG_WARN("[{}] Download {} attempt {}/{} interrupted at {} bytes: {}", TAG,
             job.version, attempt, maximum_attempts, actual_size, error);
    if (attempt == maximum_attempts) return false;

    // 10. 使用递增等待时间后继续，等待期间仍可立即响应取消请求
    set_state("downloading", "网络中断，准备断点续传（" + std::to_string(attempt + 1) + "/" +
                                 std::to_string(maximum_attempts) + "）");
    const int wait_milliseconds = std::min(attempt * 1000, 5000);
    for (int elapsed = 0; elapsed < wait_milliseconds; elapsed += 100) {
      if (canceled(stop)) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  return false;
}

/**
 * @brief 校验下载文件的 SHA-256
 * @param file 下载文件路径
 * @param expected 云端清单提供的 SHA-256
 * @param stop Host 提供的协作停止令牌
 * @param error 校验失败时写入错误信息
 * @return 校验通过返回 true
 */
bool VersionUpdatePlugin::verify_package(const fs::path& file, const std::string& expected,
                                         PluginStopToken stop, std::string& error) {
  // 1. 打开下载文件
  std::ifstream input(file, std::ios::binary);
  if (!input) {
    error = "无法读取下载文件";
    return false;
  }

  // 2. 创建并初始化 OpenSSL SHA-256 上下文
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (!context) {
    error = "无法创建 SHA-256 上下文";
    return false;
  }
  bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;

  // 3. 分块读取文件，并在每个分块前检查取消请求
  std::array<char, 1024 * 1024> buffer{};
  while (ok && input) {
    if (canceled(stop)) {
      EVP_MD_CTX_free(context);
      return false;
    }
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) ok = EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(count)) == 1;
  }

  // 4. 生成最终摘要并释放 OpenSSL 上下文
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  ok = ok && !input.bad() && EVP_DigestFinal_ex(context, digest.data(), &length) == 1;
  EVP_MD_CTX_free(context);
  if (!ok) {
    error = "计算 SHA-256 失败";
    return false;
  }

  // 5. 将摘要转换为小写十六进制字符串
  std::ostringstream actual;
  for (unsigned int index = 0; index < length; ++index)
    actual << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[index]);
  std::string expected_lower = expected;
  std::transform(expected_lower.begin(), expected_lower.end(), expected_lower.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

  // 6. 比较实际摘要和云端清单摘要
  if (actual.str() != expected_lower) {
    error = "SHA-256 校验失败";
    return false;
  }
  return true;
}

/**
 * @brief 解压软件包并安装为独立版本目录
 * @param archive_path 已通过校验的软件包路径
 * @param destination 最终版本目录
 * @param version 目标版本号
 * @param stop Host 提供的协作停止令牌
 * @param error 解压失败时写入错误信息
 * @return 解压并安装成功返回 true
 */
bool VersionUpdatePlugin::extract_package(const fs::path& archive_path, const fs::path& destination,
                                          const std::string& version, PluginStopToken stop,
                                          std::string& error) {
  // 1. 清理并重新创建本次任务的解压临时目录
  const fs::path staging = destination.parent_path() / ".staging" / version;
  std::error_code ec;
  fs::remove_all(staging, ec);
  ec.clear();
  fs::create_directories(staging, ec);
  if (ec) {
    error = "无法创建解压临时目录: " + ec.message();
    return false;
  }

  // 2. 创建 libarchive 读取器并启用 ZIP 格式支持
  struct archive* input = archive_read_new();
  if (input == nullptr) {
    error = "archive_read_new失败";
    fs::remove_all(staging, ec);
    return false;
  }
  archive_read_support_filter_all(input);
  archive_read_support_format_zip(input);

  // 3. 打开已经完成 SHA-256 校验的软件包
  const std::string archive_name = archive_path.string();
  if (archive_read_open_filename(input, archive_name.c_str(), 10240) != ARCHIVE_OK) {
    const char* detail = archive_error_string(input);
    error = "打开 ZIP 失败: " + std::string(detail == nullptr ? "未知错误" : detail);
    archive_read_free(input);
    fs::remove_all(staging, ec);
    return false;
  }

  // 4. 逐项校验路径和链接类型后解压，避免 ZIP 路径穿越
  struct archive_entry* entry = nullptr;
  int header_result = ARCHIVE_OK;
  while ((header_result = archive_read_next_header(input, &entry)) == ARCHIVE_OK) {
    if (canceled(stop)) {
      archive_read_close(input);
      archive_read_free(input);
      fs::remove_all(staging, ec);
      return false;
    }

    const char* raw_name = archive_entry_pathname(entry);
    if (raw_name == nullptr) {
      error = "ZIP 中存在非法文件名";
      archive_read_close(input);
      archive_read_free(input);
      fs::remove_all(staging, ec);
      return false;
    }

    const fs::path relative(raw_name);
    bool unsafe_path = relative.empty() || relative.is_absolute();
    for (const auto& part : relative)
      if (part == "..") unsafe_path = true;
    if (unsafe_path || archive_entry_filetype(entry) == AE_IFLNK ||
        archive_entry_hardlink(entry) != nullptr) {
      error = "ZIP 中存在不安全的路径或链接: " + std::string(raw_name);
      archive_read_close(input);
      archive_read_free(input);
      fs::remove_all(staging, ec);
      return false;
    }

    const std::string output_name = (staging / relative).string();
    archive_entry_set_pathname(entry, output_name.c_str());
    const int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM;
    if (archive_read_extract(input, entry, flags) != ARCHIVE_OK) {
      const char* detail = archive_error_string(input);
      error = "ZIP 解压失败: " + std::string(detail == nullptr ? "未知错误" : detail);
      archive_read_close(input);
      archive_read_free(input);
      fs::remove_all(staging, ec);
      return false;
    }
  }

  // 5. 检查归档读取结果并释放 libarchive 资源
  if (header_result != ARCHIVE_EOF) {
    const char* detail = archive_error_string(input);
    error = "读取 ZIP 失败: " + std::string(detail == nullptr ? "未知错误" : detail);
    archive_read_close(input);
    archive_read_free(input);
    fs::remove_all(staging, ec);
    return false;
  }
  archive_read_close(input);
  archive_read_free(input);

  // 6. 防止覆盖已经存在的本地版本
  if (fs::exists(destination, ec)) {
    error = "目标版本目录已经存在";
    fs::remove_all(staging, ec);
    return false;
  }

  // 7. 兼容压缩包直接包含文件和包含同名顶层版本目录两种结构
  fs::path extracted = staging;
  const fs::path nested = staging / version;
  if (fs::is_directory(nested, ec) && !ec) {
    std::size_t entries = 0;
    for (const auto& ignored : fs::directory_iterator(staging)) {
      (void)ignored;
      ++entries;
    }
    if (entries == 1) extracted = nested;
  }

  // 8. 在同一文件系统中原子重命名为最终版本目录
  fs::rename(extracted, destination, ec);
  if (ec) {
    error = "安装版本目录失败: " + ec.message();
    fs::remove_all(staging, ec);
    return false;
  }

  // 9. 删除嵌套目录结构留下的空临时目录
  if (extracted != staging) fs::remove_all(staging, ec);
  return true;
}

/**
 * @brief 原子切换本机当前版本
 * @param type 软件包类型
 * @param version 目标版本号
 * @param error 切换失败时写入错误信息
 * @return 切换成功返回 true
 */
bool VersionUpdatePlugin::activate_version(const std::string& type, const std::string& version,
                                           std::string& error) {
  // 1. 检查目标版本目录是否存在
  const fs::path root = package_root(type);
  const fs::path destination = root / version;
  std::error_code ec;
  if (!fs::is_directory(destination, ec) || ec) {
    error = "本地版本不存在: " + version;
    return false;
  }

  // 2. 确保部署根目录存在
  fs::create_directories(root, ec);
  if (ec) {
    error = "无法创建部署目录: " + ec.message();
    return false;
  }

  // 3. 清理上次切换可能残留的临时项
  const fs::path current = root / "current_version";
  const fs::path temporary = root / ".current_version.tmp";
  fs::remove(temporary, ec);
  ec.clear();

  // 4. 保留现有 current_version 的表示方式
  const bool use_symlink = fs::is_symlink(fs::symlink_status(current, ec));
  ec.clear();
  if (use_symlink) {
    fs::create_directory_symlink(version, temporary, ec);
  } else {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << version << '\n';
    output.flush();
    if (!output) {
      error = "写入 current_version 临时文件失败";
      fs::remove(temporary, ec);
      return false;
    }
  }

  // 5. 检查临时文件或临时软链接是否创建成功
  if (ec) {
    error = "创建 current_version 临时项失败: " + ec.message();
    return false;
  }

  // 6. 原子替换 current_version
  fs::rename(temporary, current, ec);
  if (ec) {
    error = "替换 current_version 失败: " + ec.message();
    fs::remove(temporary, ec);
    return false;
  }

  // 7. 记录版本切换结果
  LOG_INFO("[{}] {} active version switched to {}", TAG, type, version);
  return true;
}

/**
 * @brief 构建本机版本列表 JSON
 * @param type 软件包类型
 * @return 当前版本和所有本地版本信息
 */
json VersionUpdatePlugin::local_versions_json(const std::string& type) const {
  // 1. 遍历部署根目录并收集合法版本目录
  json versions = json::array();
  const fs::path root = package_root(type);
  std::error_code ec;
  if (fs::is_directory(root, ec) && !ec) {
    std::vector<std::string> names;
    for (fs::directory_iterator iterator(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
      if (iterator->is_directory(ec) && !ec) {
        const std::string name = iterator->path().filename().string();
        if (valid_version(name)) names.push_back(name);
      }
      ec.clear();
    }

    // 2. 按语义版本从高到低排序
    std::sort(names.begin(), names.end(),
              [](const auto& left, const auto& right) { return version_parts(left) > version_parts(right); });

    // 3. 计算每个版本目录大小并标记当前版本
    const std::string active = active_version(type);
    for (const auto& name : names) {
      std::error_code size_error;
      std::uint64_t size = 0;
      for (fs::recursive_directory_iterator iterator(
               root / name, fs::directory_options::skip_permission_denied, size_error), end;
           !size_error && iterator != end; iterator.increment(size_error)) {
        if (iterator->is_regular_file(size_error) && !size_error)
          size += static_cast<std::uint64_t>(iterator->file_size(size_error));
        size_error.clear();
      }
      versions.push_back({{"version", name}, {"active", name == active}, {"size", size}});
    }
  }

  // 4. 返回统一的本地版本结构
  return {{"type", type},
          {"deploy_root", root.string()},
          {"active_version", active_version(type)},
          {"versions", std::move(versions)}};
}

/**
 * @brief 读取本机当前版本
 * @param type 软件包类型
 * @return 当前版本号，无法识别时返回空字符串
 */
std::string VersionUpdatePlugin::active_version(const std::string& type) const {
  // 1. 优先读取 current_version 软链接目标
  const fs::path current = package_root(type) / "current_version";
  std::error_code ec;
  if (fs::is_symlink(fs::symlink_status(current, ec)) && !ec) {
    const auto target = fs::read_symlink(current, ec);
    if (!ec) return target.filename().string();
  }

  // 2. 普通文件模式下读取其中保存的版本号
  const std::string value = read_text_file(current);
  return valid_version(value) ? value : std::string{};
}

/**
 * @brief 从桌面 JSON 文件加载插件配置
 * @param error 加载失败时写入错误信息
 * @return 配置完整且合法返回 true
 */
bool VersionUpdatePlugin::load_config(std::string& error) {
  try {
    // 1. 默认读取 /home/codeit/Desktop/version-update-plugin.json
    const char* configured_path = std::getenv("CODEIT_UPDATE_CONFIG");
    config_path_ = configured_path && configured_path[0]
                       ? fs::path(configured_path)
                       : fs::path("/home/codeit/Desktop/version-update-plugin.json");
    std::ifstream input(config_path_, std::ios::binary);
    if (!input) {
      error = "无法读取配置文件: " + config_path_.string();
      return false;
    }

    // 2. 解析云端地址和软件包配置对象
    const json root = json::parse(input);
    configured_server_url_ = normalize_server_url(root.value("server_url", ""));
    if (!valid_server_url(configured_server_url_)) {
      error = "配置文件 server_url 必须是合法的 HTTP/HTTPS 地址";
      return false;
    }
    const json packages = root.value("packages", json::object());
    if (!packages.is_object()) {
      error = "配置文件 packages 必须是 JSON 对象";
      return false;
    }

    // 3. 加载当前插件支持的两个软件包安装目录
    std::map<std::string, PackageConfig> loaded;
    for (const std::string type : {"codeit-deploy", "frontend"}) {
      const json item = packages.value(type, json::object());
      PackageConfig config;
      config.install_path = fs::path(item.value("install_path", ""));
      config.name = item.value("name", "");
      if (config.install_path.empty() || !config.install_path.is_absolute()) {
        error = type + ".install_path 必须是绝对路径";
        return false;
      }
      if (type == "frontend" && config.name.empty()) {
        error = "frontend 必须配置 name";
        return false;
      }
      loaded.emplace(type, std::move(config));
    }
    package_configs_ = std::move(loaded);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

/**
 * @brief 从当前 Linux 系统自动识别软件包查询维度
 * @param error 识别失败时写入错误信息
 * @return 架构、平台和系统版本均可用时返回 true
 */
bool VersionUpdatePlugin::detect_system(std::string& error) {
  try {
    // 1. 通过 uname 读取运行机器架构
#if defined(__linux__)
    struct utsname information {};
    if (uname(&information) != 0) {
      error = "uname 读取系统架构失败";
      return false;
    }
    const std::string machine = lowercase(information.machine);
#elif defined(__aarch64__)
    const std::string machine = "aarch64";
#else
    const std::string machine = "x86_64";
#endif
    if (machine == "x86_64" || machine == "amd64")
      system_arch_ = "x86_64";
    else if (machine == "aarch64" || machine == "arm64")
      system_arch_ = "aarch64";
    else {
      error = "不支持的系统架构: " + machine;
      return false;
    }

    // 2. x86 使用 generic；ARM 从设备树型号识别具体板卡平台
    if (system_arch_ == "x86_64") {
      system_platform_ = "generic";
    } else {
      const std::string device = lowercase(read_text_file("/proc/device-tree/compatible") + " " +
                                           read_text_file("/proc/device-tree/model"));
      if (device.find("rk3588") != std::string::npos)
        system_platform_ = "rk3588";
      else if (device.find("tegra234") != std::string::npos ||
               device.find("orin") != std::string::npos ||
               device.find("nvidia") != std::string::npos)
        system_platform_ = "nvidia-orin";
      else if (device.find("raspberry") != std::string::npos)
        system_platform_ = "raspberry-pi";
      else {
        error = "无法从设备树识别 ARM 硬件平台";
        return false;
      }
    }

    // 3. 从 /etc/os-release 的 ID 和 VERSION_ID 生成服务端目录标识
    std::ifstream release("/etc/os-release");
    std::string os_id;
    std::string os_version;
    for (std::string line; std::getline(release, line);) {
      const auto separator = line.find('=');
      if (separator == std::string::npos) continue;
      const std::string key = line.substr(0, separator);
      const std::string value = unquote(line.substr(separator + 1));
      if (key == "ID") os_id = lowercase(value);
      if (key == "VERSION_ID") os_version = lowercase(value);
    }
    if (os_id.empty() || os_version.empty()) {
      error = "无法从 /etc/os-release 读取 ID 和 VERSION_ID";
      return false;
    }
    system_os_ = os_id + "-" + os_version;
    if (!is_safe_path_component(system_arch_) || !is_safe_path_component(system_platform_) ||
        !is_safe_path_component(system_os_)) {
      error = "系统识别结果不符合云端安全路径规则: " + system_arch_ + "/" +
              system_platform_ + "/" + system_os_;
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

/**
 * @brief 根据前端参数和本机配置构建下载任务
 * @param request 前端 JSON 参数
 * @param job 输出的任务参数
 * @param error 参数错误信息
 * @return 参数完整返回 true
 */
bool VersionUpdatePlugin::populate_job(const json& request, DownloadJob& job,
                                       std::string& error) const {
  try {
    job.type = request.value("type", "codeit-deploy");
    const auto iterator = package_configs_.find(job.type);
    if (iterator == package_configs_.end()) {
      error = "type 必须是 codeit-deploy 或 frontend";
      return false;
    }
    const PackageConfig& config = iterator->second;
    job.server_url = normalize_server_url(request.value("server_url", default_server_url()));
    job.channel = request.value("channel", "test");

    // 仅按服务端对应软件包类型填充允许的查询字段。
    if (job.type == "codeit-deploy") {
      job.arch = system_arch_;
      job.platform = system_platform_;
      job.os = system_os_;
    } else {
      job.name = config.name;
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

/**
 * @brief 将任务转换为新版云端接口需要的查询 JSON
 * @param job 下载任务
 * @return POST /api/version 使用的请求体
 */
json VersionUpdatePlugin::package_query_json(const DownloadJob& job) {
  json result = {{"type", job.type}, {"channel", job.channel}};
  if (!job.name.empty()) result["name"] = job.name;
  if (!job.arch.empty()) result["arch"] = job.arch;
  if (!job.platform.empty()) result["platform"] = job.platform;
  if (!job.os.empty()) result["os"] = job.os;
  return result;
}

/**
 * @brief 获取指定软件包类型的本机版本根目录
 * @param type 软件包类型
 * @return 配置文件声明的安装目录
 */
fs::path VersionUpdatePlugin::package_root(const std::string& type) const {
  const auto iterator = package_configs_.find(type);
  return iterator == package_configs_.end() ? fs::path{} : iterator->second.install_path;
}

/**
 * @brief 获取默认云端版本服务地址
 * @return CODEIT_UPDATE_SERVER 或配置文件中的地址
 */
std::string VersionUpdatePlugin::default_server_url() const {
  // 1. 优先读取环境变量配置
  if (const char* configured = std::getenv("CODEIT_UPDATE_SERVER"); configured && configured[0])
    return normalize_server_url(configured);

  // 2. 返回配置文件中的云端服务地址
  return configured_server_url_;
}

/**
 * @brief 检查软件包类型是否合法
 * @param type 软件包类型
 * @return 支持该类型返回 true
 */
bool VersionUpdatePlugin::valid_type(const std::string& type) {
  return type == "codeit-deploy" || type == "frontend";
}

/**
 * @brief 检查版本号格式是否合法
 * @param version 版本号
 * @return 符合 v主版本.次版本.修订号 格式返回 true
 */
bool VersionUpdatePlugin::valid_version(const std::string& version) {
  static const std::regex pattern(R"(^v[0-9]+\.[0-9]+\.[0-9]+$)");
  return std::regex_match(version, pattern);
}

/**
 * @brief 检查版本服务 URL 是否合法
 * @param url 版本服务地址
 * @return URL 格式和协议受当前构建支持时返回 true
 */
bool VersionUpdatePlugin::valid_server_url(const std::string& url) {
  static const std::regex pattern(R"(^https?://[^\s/]+(?::[0-9]+)?(?:/[^\s]*)?$)",
                                  std::regex::icase);
  return std::regex_match(url, pattern);
}

/**
 * @brief 拼接版本服务基础地址和 API 路径
 * @param server_url 版本服务基础地址
 * @param path 以斜杠开头的 API 路径
 * @return 完整 URL
 */
std::string VersionUpdatePlugin::build_url(const std::string& server_url,
                                           const std::string& path) {
  if (path.empty() || path.front() != '/') throw std::invalid_argument("非法 URL path");
  return normalize_server_url(server_url) + path;
}

/**
 * @brief 将 libcurl 接收的数据追加到字符串
 * @param data libcurl 提供的数据缓冲区
 * @param size 单个数据块大小
 * @param count 数据块数量
 * @param user_data 目标字符串指针
 * @return 实际接收的字节数
 */
std::size_t VersionUpdatePlugin::curl_write_string(char* data, std::size_t size,
                                                   std::size_t count, void* user_data) {
  const std::size_t total = size * count;
  static_cast<std::string*>(user_data)->append(data, total);
  return total;
}

/**
 * @brief 将 libcurl 接收的数据写入临时文件
 * @param data libcurl 提供的数据缓冲区
 * @param size 单个数据块大小
 * @param count 数据块数量
 * @param user_data 下载回调上下文
 * @return 实际写入的字节数；取消或写入失败时小于请求字节数
 */
std::size_t VersionUpdatePlugin::curl_write_file(char* data, std::size_t size,
                                                 std::size_t count, void* user_data) {
  auto* context = static_cast<CurlDownloadContext*>(user_data);
  if (context->plugin->canceled(context->stop)) return 0;
  return std::fwrite(data, size, count, context->file) * size;
}

/**
 * @brief 更新 libcurl 下载进度并观察取消请求
 * @param user_data 下载回调上下文
 * @param total libcurl 报告的下载总字节数
 * @param now 当前已下载字节数
 * @return 继续下载返回 0，取消下载返回非 0
 */
int VersionUpdatePlugin::curl_progress(void* user_data, curl_off_t total, curl_off_t now,
                                       curl_off_t, curl_off_t) {
  auto* context = static_cast<CurlDownloadContext*>(user_data);
  if (context->plugin->canceled(context->stop)) return 1;
  {
    std::lock_guard<std::mutex> lock(context->plugin->mutex_);
    context->plugin->downloaded_bytes_ =
        context->resume_offset + (now > 0 ? static_cast<std::uint64_t>(now) : 0);
    if (total > 0)
      context->plugin->total_bytes_ = context->resume_offset + static_cast<std::uint64_t>(total);
  }
  context->plugin->broadcast_status(false);
  return 0;
}

/**
 * @brief 解析插件 HTTP 请求体
 * @param request Host 转发的 HTTP 请求
 * @return JSON 对象；空请求体返回空对象
 */
json VersionUpdatePlugin::parse_body(const PluginHttpRequest& request) {
  // 1. 空请求体转换为空 JSON 对象
  if (request.text().empty()) return json::object();

  // 2. 解析 JSON 并限制顶层类型为对象
  json body = json::parse(request.text());
  if (!body.is_object()) throw std::invalid_argument("请求体必须是 JSON 对象");
  return body;
}

/**
 * @brief 构建 JSON HTTP 响应
 * @param status HTTP 状态码
 * @param body JSON 响应体
 * @return 插件 HTTP 响应
 */
PluginHttpResponse VersionUpdatePlugin::json_response(int status, const json& body) {
  // 1. 设置状态码和 JSON 内容类型
  PluginHttpResponse response;
  response.status_code = status;
  response.content_type = "application/json; charset=utf-8";

  // 2. 序列化 JSON 并写入响应体
  const std::string text = body.dump();
  response.body.assign(text.begin(), text.end());
  return response;
}

/**
 * @brief 构建统一的 JSON 错误响应
 * @param status HTTP 状态码
 * @param code 机器可读错误码
 * @param message 用户可读错误说明
 * @return 插件 HTTP 错误响应
 */
PluginHttpResponse VersionUpdatePlugin::json_error(int status, const std::string& code,
                                                   const std::string& message) {
  return json_response(status, {{"code", -1}, {"error", code}, {"message", message}});
}

/**
 * @brief 构建当前任务和所有本机软件包的状态快照
 * @return WebSocket 与 HTTP 共用的状态 JSON
 */
json VersionUpdatePlugin::status_snapshot_json() const {
  json result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const double progress = total_bytes_ == 0
                                ? 0.0
                                : 100.0 * static_cast<double>(downloaded_bytes_) /
                                      static_cast<double>(total_bytes_);
    result = {{"type", "status"},
              {"state", state_},
              {"message", message_},
              {"target_type", target_type_},
              {"target_version", target_version_},
              {"downloaded_bytes", downloaded_bytes_},
              {"total_bytes", total_bytes_},
              {"progress", std::min(100.0, progress)}};
  }

  // 当前版本读取不持有任务互斥锁，避免文件系统访问阻塞下载进度更新。
  json packages = json::object();
  for (const std::string type : {"codeit-deploy", "frontend"}) {
    packages[type] = {{"active_version", active_version(type)},
                      {"deploy_root", package_root(type).string()}};
  }
  result["packages"] = std::move(packages);
  result["system"] = {{"arch", system_arch_},
                      {"platform", system_platform_},
                      {"os", system_os_}};
  return result;
}

/**
 * @brief 向所有 WebSocket 客户端推送最新状态
 * @param force true 表示忽略进度推送节流
 */
void VersionUpdatePlugin::broadcast_status(bool force) noexcept {
  try {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    if (!force) {
      const auto previous = last_ws_progress_ms_.load(std::memory_order_relaxed);
      if (now - previous < 200) return;
      last_ws_progress_ms_.store(now, std::memory_order_relaxed);
    }
    ws_broadcast_latest_text(kEndpoint, "version_update.status", status_snapshot_json().dump());
  } catch (const std::exception& exception) {
    LOG_WARN("[{}] Failed to broadcast WebSocket status: {}", TAG, exception.what());
  } catch (...) {
    LOG_WARN("[{}] Failed to broadcast WebSocket status", TAG);
  }
}

/**
 * @brief 更新当前任务状态
 * @param state 新的任务阶段
 * @param message 新的任务提示或错误信息
 */
void VersionUpdatePlugin::set_state(const std::string& state, const std::string& message) {
  // 使用同一互斥锁保证状态和提示信息同时更新
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    message_ = message;
  }
  broadcast_status(true);
}

/**
 * @brief 判断当前操作是否应当取消
 * @param stop Host 提供的协作停止令牌
 * @return Host 请求停止或用户请求取消时返回 true
 */
bool VersionUpdatePlugin::canceled(PluginStopToken stop) const {
  return stop.stop_requested() || cancel_requested_.load();
}

PLUGIN_DECLARE(VersionUpdatePlugin, "VersionUpdatePlugin", "1.0", "liuyan",
               "Codeit local version download and switch plugin")
