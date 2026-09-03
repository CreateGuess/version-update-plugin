#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include <curl/curl.h>

#include "nlohmann/json.hpp"
#include "plugin_core/api/plugin_websocket_api.hpp"
#include "plugin_core/sdk/plugin_logx.hpp"
#include "plugin_core/sdk/plugin_sdk.hpp"

/**
 * @brief Codeit 本地版本下载与切换插件
 *
 * 通过 rpc_gateway 暴露 HTTP 接口，负责查询云端版本、异步下载软件包、
 * 校验与解压软件包，以及维护本机 current_version。
 */
class VersionUpdatePlugin final : public PluginBase {
 public:
  VersionUpdatePlugin() = default;
  ~VersionUpdatePlugin() override = default;

 private:
  /**
   * @brief 前端创建的下载任务参数
   */
  struct DownloadJob {
    std::string type;        // 软件包类型：codeit-deploy 或 frontend
    std::string version;     // 目标版本，如 v1.3.50
    std::string server_url;  // 云端版本服务地址
    std::string arch;        // 软件包架构
    std::string name;        // backend/frontend 的组件名
    std::string platform;    // 硬件平台
    std::string os;          // 操作系统版本
    std::string channel;     // 发布通道
    bool activate{false};    // 下载完成后是否立即切换
  };

  /**
   * @brief 从本机配置文件读取的软件包配置
   */
  struct PackageConfig {
    std::filesystem::path install_path; // 本机版本安装根目录
    std::string name;                   // frontend/backend 组件名
  };

  /**
   * @brief libcurl 下载回调使用的上下文
   */
  struct CurlDownloadContext {
    VersionUpdatePlugin* plugin; // 当前插件实例
    std::FILE* file;             // 临时下载文件
    PluginStopToken stop;        // Host 协作停止令牌
    std::uint64_t resume_offset; // 本次请求开始前已经下载的字节数
  };

  /**
   * @brief 从云端版本清单解析出的软件包信息
   */
  struct PackageInfo {
    std::string version;       // 软件包版本
    std::string download_url;  // 相对于版本服务器的下载接口
    nlohmann::json download_body; // 下载接口要求的 JSON 请求体
    std::string sha256;        // 软件包 SHA-256
    std::uint64_t size{0};     // 软件包字节数
  };

  // 初始化插件日志
  bool on_init(IPluginContext& ctx) noexcept override;
  // 注册 HTTP 接口并启动 Host 托管的下载线程
  bool on_start() noexcept override;
  // 请求取消任务并停止插件
  void on_stop() noexcept override;
  // 销毁插件日志资源
  void on_unload() noexcept override;

  // 统一处理版本管理 HTTP 请求
  PluginHttpResponse handle_http(const PluginHttpRequest& request);
  // 获取当前下载任务状态
  PluginHttpResponse handle_status(const PluginHttpRequest& request);
  // 获取本地版本列表
  PluginHttpResponse handle_local_versions(const PluginHttpRequest& request);
  // 查询云端版本清单
  PluginHttpResponse handle_remote_versions(const nlohmann::json& request);
  // 创建异步下载任务
  PluginHttpResponse handle_download(const nlohmann::json& request);
  // 取消当前下载任务
  PluginHttpResponse handle_cancel();
  // 切换到指定本地版本
  PluginHttpResponse handle_switch(const nlohmann::json& request);
  // 处理 WebSocket 客户端消息
  void handle_ws_message(const char* session_id, const void* data, std::size_t size,
                         PluginWsMessageType type);
  // WebSocket 建立连接时发送完整状态快照
  void handle_ws_open(const char* session_id);

  // Host 托管的下载任务循环
  void worker_loop(PluginStopToken stop);
  // 执行一次完整的下载、校验、解压和可选切换流程
  void execute_download(const DownloadJob& job, PluginStopToken stop);
  // 查询并解析目标软件包信息
  std::optional<PackageInfo> fetch_package(const DownloadJob& job, std::string& error);
  // 将云端软件包流式写入临时文件
  bool download_package(const DownloadJob& job, const PackageInfo& package,
                        const std::filesystem::path& temporary_file, PluginStopToken stop,
                        std::string& error);
  // 校验下载文件的 SHA-256
  bool verify_package(const std::filesystem::path& file, const std::string& expected,
                      PluginStopToken stop, std::string& error);
  // 解压并原子安装版本目录
  bool extract_package(const std::filesystem::path& archive_path,
                       const std::filesystem::path& destination, const std::string& version,
                       PluginStopToken stop, std::string& error);
  // 原子更新 current_version
  bool activate_version(const std::string& type, const std::string& version, std::string& error);

  // 构建本地版本列表 JSON
  nlohmann::json local_versions_json(const std::string& type) const;
  // 读取 current_version 指向的当前版本
  std::string active_version(const std::string& type) const;
  // 获取指定软件包类型的本机版本根目录
  std::filesystem::path package_root(const std::string& type) const;
  // 从 /home/codeit/Desktop 配置文件加载服务器地址和安装路径
  bool load_config(std::string& error);
  // 从 Linux 系统信息识别架构、硬件平台和操作系统版本
  bool detect_system(std::string& error);
  // 根据前端参数和本机配置构建下载任务
  bool populate_job(const nlohmann::json& request, DownloadJob& job, std::string& error) const;
  // 将任务的云端查询维度转换为 JSON
  static nlohmann::json package_query_json(const DownloadJob& job);
  // 获取默认云端版本服务地址
  std::string default_server_url() const;
  // 检查软件包类型是否合法
  static bool valid_type(const std::string& type);
  // 检查版本号是否合法
  static bool valid_version(const std::string& version);
  // 检查云端服务 URL 是否合法
  static bool valid_server_url(const std::string& url);
  // 拼接云端服务基础地址和 API 路径
  static std::string build_url(const std::string& server_url, const std::string& path);
  // libcurl 字符串写入回调
  static std::size_t curl_write_string(char* data, std::size_t size, std::size_t count,
                                       void* user_data);
  // libcurl 文件写入回调
  static std::size_t curl_write_file(char* data, std::size_t size, std::size_t count,
                                     void* user_data);
  // libcurl 下载进度和取消回调
  static int curl_progress(void* user_data, curl_off_t total, curl_off_t now, curl_off_t,
                           curl_off_t);
  // 解析 HTTP JSON 请求体
  static nlohmann::json parse_body(const PluginHttpRequest& request);
  // 构建 JSON HTTP 响应
  static PluginHttpResponse json_response(int status, const nlohmann::json& body);
  // 构建统一的 JSON 错误响应
  static PluginHttpResponse json_error(int status, const std::string& code,
                                       const std::string& message);
  // 构建可通过 HTTP 或 WebSocket 返回的状态快照
  nlohmann::json status_snapshot_json() const;
  // 向所有 WebSocket 客户端推送最新状态
  void broadcast_status(bool force = true) noexcept;

  // 更新当前任务状态和提示信息
  void set_state(const std::string& state, const std::string& message = {});
  // 判断插件停止或用户取消是否已经发生
  bool canceled(PluginStopToken stop) const;

 private:
  const std::string TAG = "版本更新插件";       // 日志标签
  std::atomic_bool started_{false};             // 插件是否已经启动
  std::atomic_bool cancel_requested_{false};    // 用户是否请求取消任务
  mutable std::mutex mutex_;                    // 保护任务状态和版本清单
  std::condition_variable job_cv_;              // 唤醒下载工作线程
  std::optional<DownloadJob> pending_job_;      // 等待执行的下载任务
  std::string state_{"idle"};                  // 当前任务阶段
  std::string message_;                         // 当前任务提示或错误信息
  std::string target_type_;                     // 当前任务的软件包类型
  std::string target_version_;                  // 当前任务的目标版本
  std::uint64_t downloaded_bytes_{0};           // 已下载字节数
  std::uint64_t total_bytes_{0};                // 软件包总字节数
  nlohmann::json remote_manifest_;              // 最近一次查询到的云端版本清单
  std::filesystem::path config_path_;            // 当前使用的本机配置文件
  std::string configured_server_url_;            // 配置文件中的云端服务地址
  std::map<std::string, PackageConfig> package_configs_; // 各软件包安装与查询配置
  std::string system_arch_;                       // 系统自动识别的软件包架构
  std::string system_platform_;                   // 系统自动识别的硬件平台
  std::string system_os_;                         // 系统自动识别的操作系统版本
  std::atomic<std::int64_t> last_ws_progress_ms_{0}; // 最近一次进度推送时间
};
