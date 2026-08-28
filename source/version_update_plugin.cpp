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

  // 2. 初始化 libcurl 全局环境
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    LOG_ERROR("[{}] curl_global_init failed", TAG);
    LOGX_GLOBAL_CLASS(PLUGIN_LOG_NAME)::Destruct();
    return false;
  }

  // 3. 输出两类软件包的本机版本目录
  LOG_INFO("[{}] Initialized; codeit-deploy root: {}, frontend root: {}", TAG,
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

  // 4. 启动由 Host 管理的下载工作线程
  cancel_requested_.store(false);
  if (!start_managed_worker("version-update-download",
                            [this](PluginStopToken stop) { worker_loop(stop); })) {
    LOG_ERROR("[{}] Failed to start download worker", TAG);
    started_.store(false);
    return false;
  }

  // 5. 插件启动完成
  LOG_INFO("[{}] Started: /backend/plugin-http/{}/...", TAG, kEndpoint);
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

  // 2. 在互斥锁保护下复制任务状态并计算下载百分比
  json result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const double progress = total_bytes_ == 0
                                ? 0.0
                                : 100.0 * static_cast<double>(downloaded_bytes_) /
                                      static_cast<double>(total_bytes_);
    result = {{"state", state_}, {"message", message_}, {"target_type", target_type_},
              {"target_version", target_version_},
              {"downloaded_bytes", downloaded_bytes_}, {"total_bytes", total_bytes_},
              {"progress", std::min(100.0, progress)}};
  }

  // 3. 补充实时读取的当前版本和部署目录
  result["selected_type"] = selected_type;
  result["active_version"] = active_version(selected_type);
  result["deploy_root"] = package_root(selected_type).string();
  return json_response(200, {{"code", 0}, {"message", "ok"}, {"data", std::move(result)}});
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
  // 1. 读取请求参数，未提供时使用本机默认配置
  DownloadJob job;
  job.type = request.value("type", "codeit-deploy");
  job.server_url = trim_trailing_slashes(request.value("server_url", default_server_url()));
  job.arch = request.value("arch", native_arch());
  job.channel = request.value("channel", "release");

  // 2. 校验服务器地址、架构和发布通道
  if (!valid_type(job.type))
    return json_error(400, "invalid_type", "type 必须是 codeit-deploy 或 frontend");
  if (!valid_server_url(job.server_url))
    return json_error(400, "invalid_server_url", "server_url 必须是合法的 http:// 或 https:// 地址");
  if (!valid_arch(job.arch))
    return json_error(400, "invalid_arch", "arch 必须是 x86_64、aarch64 或 nvidia-orin");
  if (job.channel != "test" && job.channel != "release")
    return json_error(400, "invalid_channel", "channel 必须是 test 或 release");

  // 3. 从云端读取并缓存版本清单
  std::string error;
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
 * @param request 前端提交的版本、架构、通道和自动切换参数
 * @return 任务创建结果 HTTP 响应
 */
PluginHttpResponse VersionUpdatePlugin::handle_download(const json& request) {
  // 1. 读取下载任务参数
  DownloadJob job;
  job.type = request.value("type", "codeit-deploy");
  job.version = request.value("version", "");
  job.server_url = trim_trailing_slashes(request.value("server_url", default_server_url()));
  job.arch = request.value("arch", native_arch());
  job.channel = request.value("channel", "release");
  job.activate = request.value("activate", false);

  // 2. 校验版本号、服务器地址、架构和发布通道
  if (!valid_type(job.type))
    return json_error(400, "invalid_type", "type 必须是 codeit-deploy 或 frontend");
  if (!valid_version(job.version))
    return json_error(400, "invalid_version", "version 格式必须是 v主版本.次版本.修订号");
  if (!valid_server_url(job.server_url))
    return json_error(400, "invalid_server_url", "server_url 必须是合法的 http:// 或 https:// 地址");
  if (!valid_arch(job.arch))
    return json_error(400, "invalid_arch", "arch 必须是 x86_64、aarch64 或 nvidia-orin");
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

    // 2. 构建版本清单 URL 和响应缓冲区
    const std::string url =
        build_url(job.server_url,
                  "/api/version/" + job.type + "/" + job.arch + "/" + job.channel);
    std::string body;
    char error_buffer[CURL_ERROR_SIZE]{};

    // 3. 配置重定向、超时、线程安全和 TLS 校验
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
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
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
      error = std::string("获取云端版本失败: ") +
              (error_buffer[0] ? error_buffer : curl_easy_strerror(result));
      if (status != 0) error += " (HTTP " + std::to_string(status) + ")";
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
      PackageInfo package{job.version, item.value("url", ""), item.value("sha256", ""),
                          item.value("size", std::uint64_t{0})};
      if (package.url.empty() || package.url.front() != '/' || package.sha256.size() != 64) {
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
  const std::string url = build_url(job.server_url, package.url);
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

    // 5. 配置 HTTP Range、连接保活、流式写入、进度回调和取消处理
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
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
 * @brief 获取指定软件包类型的本机版本根目录
 * @param type 软件包类型
 * @return 环境变量配置或根据用户和架构生成的默认目录
 */
fs::path VersionUpdatePlugin::package_root(const std::string& type) const {
  // 1. 优先使用不同软件包类型对应的环境变量
  const char* variable = type == "frontend" ? "CODEIT_FRONTEND_ROOT" : "CODEIT_DEPLOY_ROOT";
  if (const char* configured = std::getenv(variable); configured && configured[0])
    return fs::path(configured);

  // 2. 确定实际部署用户目录。rpc_gateway 经常以 root 运行，不能使用 /root。
  fs::path user_home;
  if (const char* home = std::getenv("HOME"); home && home[0]) {
    const fs::path home_path(home);
    if (home_path != fs::path("/root")) user_home = home_path;
  }
  if (user_home.empty())
    user_home = native_arch() == "x86_64" ? fs::path("/home/codeit") : fs::path("/home/pi");

  // 3. frontend 使用 robot-platform 项目目录，codeit-deploy 目录包含本机架构后缀
  if (type == "frontend")
    return user_home / "Desktop" / "frontend" / "robot-platform";
  return user_home / "Desktop" / ("codeit-deploy_" + native_arch());
}

/**
 * @brief 获取默认云端版本服务地址
 * @return CODEIT_UPDATE_SERVER 或本机 28000 端口地址
 */
std::string VersionUpdatePlugin::default_server_url() const {
  // 1. 优先读取环境变量配置
  if (const char* configured = std::getenv("CODEIT_UPDATE_SERVER"); configured && configured[0])
    return trim_trailing_slashes(configured);

  // 2. 返回本机 Nginx 的默认地址
  return "http://127.0.0.1:28000";
}

/**
 * @brief 获取当前编译目标的软件包架构
 * @return aarch64 或 x86_64
 */
std::string VersionUpdatePlugin::native_arch() {
#if defined(__aarch64__)
  return "aarch64";
#else
  return "x86_64";
#endif
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
 * @brief 检查软件包架构是否合法
 * @param arch 软件包架构
 * @return 支持该架构返回 true
 */
bool VersionUpdatePlugin::valid_arch(const std::string& arch) {
  return arch == "x86_64" || arch == "aarch64" || arch == "nvidia-orin";
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
  return trim_trailing_slashes(server_url) + path;
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
  std::lock_guard<std::mutex> lock(context->plugin->mutex_);
  context->plugin->downloaded_bytes_ =
      context->resume_offset + (now > 0 ? static_cast<std::uint64_t>(now) : 0);
  if (total > 0)
    context->plugin->total_bytes_ = context->resume_offset + static_cast<std::uint64_t>(total);
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
 * @brief 更新当前任务状态
 * @param state 新的任务阶段
 * @param message 新的任务提示或错误信息
 */
void VersionUpdatePlugin::set_state(const std::string& state, const std::string& message) {
  // 使用同一互斥锁保证状态和提示信息同时更新
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = state;
  message_ = message;
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
