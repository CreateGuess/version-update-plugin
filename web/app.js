"use strict";

const $ = (selector) => document.querySelector(selector);
const elements = {
  apiBase: $("#apiBase"),
  serverUrl: $("#serverUrl"),
  packageType: $("#packageType"),
  packageOptions: $(".package-options"),
  packageHint: $("#packageHint"),
  channel: $("#channel"),
  systemProfile: $("#systemProfile"),
  connection: $("#connectionState"),
  connectionText: $("#connectionText"),
  activeVersion: $("#heroActiveVersion"),
  deployRoot: $("#deployRoot"),
  taskState: $("#taskState"),
  targetVersion: $("#targetVersion"),
  taskMessage: $("#taskMessage"),
  progressValue: $("#progressValue"),
  progressTrack: $("#progressTrack"),
  progressBar: $("#progressBar"),
  downloadBytes: $("#downloadBytes"),
  cancelButton: $("#cancelButton"),
  phaseList: $("#phaseList"),
  remoteVersions: $("#remoteVersions"),
  remoteTitle: $("#remoteTitle"),
  localVersions: $("#localVersions"),
  localTitle: $("#localTitle"),
  refreshRemoteButton: $("#refreshRemoteButton"),
  refreshLocalButton: $("#refreshLocalButton"),
  saveSettingsButton: $("#saveSettingsButton"),
  activityLog: $("#activityLog"),
  clearLogButton: $("#clearLogButton"),
  toastRegion: $("#toastRegion"),
};

const phases = ["checking", "downloading", "verifying", "extracting", "activating"];
const busyStates = new Set(["queued", ...phases]);
const stateLabels = {
  idle: "空闲", queued: "排队中", checking: "检查中", downloading: "下载中",
  verifying: "校验中", extracting: "解压中", activating: "切换中",
  completed: "已完成", failed: "失败", canceled: "已取消",
};

let statusSocket = null;
let reconnectTimer = null;
let lastState = "";
let localVersionNames = new Set();
let remotePackages = [];
let packageCatalog = [];

function defaultApiBase() {
  const host = window.location.hostname || "127.0.0.1";
  return `http://${host}/backend/plugin-http/version_update`;
}

function statusWebSocketUrl() {
  const apiUrl = new URL(elements.apiBase.value.trim());
  apiUrl.protocol = apiUrl.protocol === "https:" ? "wss:" : "ws:";
  apiUrl.pathname = apiUrl.pathname.replace(
    /\/backend\/plugin-http\/version_update\/?$/,
    "/backend/plugin-ws/version_update",
  );
  apiUrl.search = "";
  apiUrl.hash = "";
  return apiUrl.toString();
}

function connectStatusSocket() {
  window.clearTimeout(reconnectTimer);
  if (statusSocket) {
    statusSocket.onclose = null;
    statusSocket.close();
  }

  try {
    const socket = new WebSocket(statusWebSocketUrl());
    statusSocket = socket;
    socket.addEventListener("open", () => {
      setConnection(true);
      socket.send("snapshot");
    });
    socket.addEventListener("message", (event) => {
      try {
        const message = JSON.parse(event.data);
        if (message.type !== "status") return;
        const selected = elements.packageType.value;
        const packageState = message.packages?.[selected] || {};
        const status = { ...message, ...packageState, selected_type: selected };
        renderStatus(status);
        if (lastState && status.state !== lastState) {
          logActivity(`${stateLabels[status.state] || status.state}：${status.message || status.target_version || "任务状态已更新"}`);
          if (["completed", "failed", "canceled"].includes(status.state))
            refreshLocalVersions(true);
        }
        lastState = status.state;
      } catch (error) {
        logActivity(`状态推送解析失败：${error.message}`);
      }
    });
    socket.addEventListener("close", () => {
      if (statusSocket !== socket) return;
      setConnection(false, "WebSocket 已断开，正在重连");
      reconnectTimer = window.setTimeout(connectStatusSocket, 2000);
    });
    socket.addEventListener("error", () => socket.close());
  } catch (error) {
    setConnection(false, error.message);
    reconnectTimer = window.setTimeout(connectStatusSocket, 2000);
  }
}

function loadSettings() {
  elements.apiBase.value = localStorage.getItem("codeit.apiBase") || defaultApiBase();
  elements.serverUrl.value = localStorage.getItem("codeit.serverUrl") || "";
  elements.packageType.value = localStorage.getItem("codeit.packageType") || "codeit-deploy";
  elements.channel.value = localStorage.getItem("codeit.channel") || "test";
  renderPackageContext();
}

function renderPackageContext() {
  const selected = packageCatalog.find((item) => item.id === elements.packageType.value);
  const frontend = selected?.type === "frontend";
  const label = selected?.label || elements.packageType.value;
  document.body.dataset.packageType = frontend ? "frontend" : "codeit-deploy";
  elements.packageHint.textContent = selected
    ? `${selected.type}${selected.name ? ` / ${selected.name}` : ""} · ${selected.install_path}`
    : "等待读取软件包配置";
  elements.remoteTitle.textContent = `${label} 可用版本`;
  elements.localTitle.textContent = `${label} 已下载版本`;
  elements.packageOptions.querySelectorAll("[data-package-type]").forEach((button) => {
    const active = button.dataset.packageType === elements.packageType.value;
    button.classList.toggle("active", active);
    button.setAttribute("aria-pressed", String(active));
  });
}

function renderPackageSelector() {
  elements.packageOptions.replaceChildren();
  packageCatalog.forEach((item) => {
    const button = document.createElement("button");
    button.type = "button";
    button.dataset.packageType = item.id;
    const title = document.createElement("strong");
    title.textContent = item.label || item.id;
    const detail = document.createElement("small");
    detail.textContent = item.name || item.type;
    button.append(title, detail);
    button.addEventListener("click", () => selectPackageType(item.id));
    elements.packageOptions.append(button);
  });
  renderPackageContext();
}

async function refreshPackageCatalog() {
  packageCatalog = await api("/packages");
  if (!Array.isArray(packageCatalog) || !packageCatalog.length)
    throw new Error("插件配置中没有可管理的软件包");
  if (!packageCatalog.some((item) => item.id === elements.packageType.value))
    elements.packageType.value = packageCatalog[0].id;
  renderPackageSelector();
}

function selectPackageType(type) {
  if (!packageCatalog.some((item) => item.id === type)) return;
  elements.packageType.value = type;
  renderPackageContext();
  saveSettings(false);
  remotePackages = [];
  localVersionNames = new Set();
  renderEmpty(elements.remoteVersions, "尚未查询云端版本", "点击“查询云端版本”刷新当前类型");
  refreshAll();
}

function saveSettings(showMessage = true) {
  const apiBase = elements.apiBase.value.trim().replace(/\/+$/, "");
  const serverUrl = elements.serverUrl.value.trim().replace(/\/+$/, "");
  elements.apiBase.value = apiBase;
  elements.serverUrl.value = serverUrl;
  localStorage.setItem("codeit.apiBase", apiBase);
  localStorage.setItem("codeit.serverUrl", serverUrl);
  localStorage.setItem("codeit.packageType", elements.packageType.value);
  localStorage.setItem("codeit.channel", elements.channel.value);
  if (showMessage) {
    toast("连接配置已保存");
    logActivity("更新了连接配置");
    refreshPackageCatalog()
      .then(() => refreshAll())
      .then(() => connectStatusSocket())
      .catch((error) => {
        setConnection(false, error.message);
        toast(error.message, true);
      });
  }
}

async function api(path, options = {}) {
  const base = elements.apiBase.value.trim().replace(/\/+$/, "");
  if (!/^https?:\/\//i.test(base)) throw new Error("请填写有效的本机插件 API 地址");
  const init = { method: options.method || "GET", headers: { Accept: "application/json" } };
  if (options.body !== undefined) {
    init.headers["Content-Type"] = "application/json";
    init.body = JSON.stringify(options.body);
  }
  const response = await fetch(`${base}${path}`, init);
  let payload;
  try { payload = await response.json(); }
  catch { throw new Error(`服务返回了无法解析的响应（HTTP ${response.status}）`); }
  if (!response.ok || payload.code === -1) {
    throw new Error(payload.message || payload.error || `请求失败（HTTP ${response.status}）`);
  }
  return payload.data ?? payload;
}

function requestContext() {
  const context = {
    package: elements.packageType.value,
    channel: elements.channel.value,
  };
  const serverUrl = elements.serverUrl.value.trim();
  if (serverUrl) context.server_url = serverUrl;
  return context;
}

async function refreshStatus(silent = false) {
  try {
    const status = await api(`/status?package=${encodeURIComponent(elements.packageType.value)}`);
    setConnection(true);
    renderStatus(status);
    if (lastState && status.state !== lastState) {
      logActivity(`${stateLabels[status.state] || status.state}：${status.message || status.target_version || "任务状态已更新"}`);
      if (["completed", "failed", "canceled"].includes(status.state)) refreshLocalVersions(true);
    }
    lastState = status.state;
    return status;
  } catch (error) {
    setConnection(false, error.message);
    if (!silent) toast(error.message, true);
    return null;
  }
}

function renderStatus(status) {
  const progress = Math.max(0, Math.min(100, Number(status.progress) || 0));
  elements.activeVersion.textContent = status.active_version || "未设置";
  elements.deployRoot.textContent = status.deploy_root || "部署目录未知";
  elements.taskState.textContent = stateLabels[status.state] || status.state || "未知";
  elements.taskState.dataset.state = status.state || "idle";
  elements.targetVersion.textContent = status.target_version || "暂无任务";
  elements.taskMessage.textContent = status.message || "可以从下方选择云端版本开始下载。";
  elements.progressValue.textContent = Math.round(progress).toString();
  elements.progressBar.style.width = `${progress}%`;
  elements.progressTrack.setAttribute("aria-valuenow", String(Math.round(progress)));
  elements.downloadBytes.textContent = `${formatBytes(status.downloaded_bytes)} / ${formatBytes(status.total_bytes)}`;
  const system = status.system || {};
  elements.systemProfile.textContent = [system.arch, system.platform, system.os].filter(Boolean).join(" · ") || "等待插件识别";
  elements.cancelButton.disabled = !busyStates.has(status.state);
  renderPhases(status.state);
  document.querySelectorAll(".download-action, .switch-action").forEach((button) => {
    button.disabled = busyStates.has(status.state);
  });
}

function renderPhases(currentState) {
  const currentIndex = phases.indexOf(currentState);
  elements.phaseList.querySelectorAll("li").forEach((item, index) => {
    item.classList.toggle("active", index === currentIndex);
    item.classList.toggle("done", currentState === "completed" || index < currentIndex);
  });
}

async function refreshRemoteVersions() {
  saveSettings(false);
  const type = elements.packageType.value;
  elements.refreshRemoteButton.disabled = true;
  elements.refreshRemoteButton.textContent = "正在查询…";
  try {
    const manifest = await api("/remote", { method: "POST", body: requestContext() });
    remotePackages = manifest.packages || [];
    renderRemoteVersions(remotePackages);
    logActivity(`${type} 查询到 ${manifest.packages?.length || 0} 个云端版本`);
    toast(`${type} 云端版本已更新`);
  } catch (error) {
    renderEmpty(elements.remoteVersions, "云端查询失败", error.message);
    toast(error.message, true);
    logActivity(`云端查询失败：${error.message}`);
  } finally {
    elements.refreshRemoteButton.disabled = false;
    elements.refreshRemoteButton.textContent = "查询云端版本";
  }
}

function renderRemoteVersions(packages) {
  elements.remoteVersions.replaceChildren();
  if (!packages.length) return renderEmpty(elements.remoteVersions, "没有可用版本", "当前通道尚未发布软件包");
  packages.forEach((pkg) => {
    const card = createVersionCard(pkg.version, {
      tag: pkg.is_latest ? "LATEST / 最新" : "PREVIOUS / 上一版本",
      metadata: [`${formatBytes(pkg.size)}`, shortHash(pkg.sha256)],
      active: false,
    });
    const actions = document.createElement("div");
    actions.className = "version-actions";

    const activateLabel = document.createElement("label");
    const activate = document.createElement("input");
    activate.type = "checkbox";
    activate.checked = pkg.is_latest === true;
    activateLabel.append(activate, document.createTextNode("下载后切换"));

    const button = document.createElement("button");
    button.type = "button";
    button.className = "card-button download-action";
    const installed = localVersionNames.has(pkg.version.replace(/\.zip$/, ""));
    button.textContent = installed ? "已下载" : "下载版本 →";
    button.disabled = installed || busyStates.has(lastState);
    button.addEventListener("click", () => startDownload(pkg.version, activate.checked));
    actions.append(activateLabel, button);
    card.append(actions);
    elements.remoteVersions.append(card);
  });
}

async function startDownload(version, activate) {
  const type = elements.packageType.value;
  try {
    await api("/download", {
      method: "POST",
      body: { ...requestContext(), version, activate },
    });
    toast(`${type} ${version} 已加入下载队列`);
    logActivity(`开始下载 ${type} ${version}${activate ? "，完成后自动切换" : ""}`);
    await refreshStatus(true);
  } catch (error) {
    toast(error.message, true);
    logActivity(`下载任务创建失败：${error.message}`);
  }
}

async function cancelDownload() {
  elements.cancelButton.disabled = true;
  try {
    await api("/cancel", { method: "POST", body: {} });
    toast("取消请求已提交");
    logActivity("请求取消当前下载任务");
  } catch (error) {
    toast(error.message, true);
  }
}

async function refreshLocalVersions(silent = false) {
  elements.refreshLocalButton.disabled = true;
  try {
    const data = await api(`/versions?package=${encodeURIComponent(elements.packageType.value)}`);
    setConnection(true);
    renderLocalVersions(data.versions || [], data.active_version || "");
  } catch (error) {
    setConnection(false, error.message);
    renderEmpty(elements.localVersions, "无法读取本地版本", error.message);
    if (!silent) toast(error.message, true);
  } finally {
    elements.refreshLocalButton.disabled = false;
  }
}

function renderLocalVersions(versions, activeVersion) {
  localVersionNames = new Set(versions.map((item) => item.version));
  elements.localVersions.replaceChildren();
  if (!versions.length) {
    renderEmpty(elements.localVersions, "尚无本地版本", "请先从云端下载一个版本");
    if (remotePackages.length) renderRemoteVersions(remotePackages);
    return;
  }
  versions.forEach((item) => {
    const isActive = item.active || item.version === activeVersion;
    const card = createVersionCard(item.version, {
      tag: isActive ? "ACTIVE / 当前版本" : "LOCAL / 本地版本",
      metadata: [formatBytes(item.size), isActive ? "正在使用" : "可随时切换"],
      active: isActive,
    });
    const actions = document.createElement("div");
    actions.className = "version-actions";
    const hint = document.createElement("span");
    hint.className = "tag";
    hint.textContent = isActive ? "CURRENT" : "READY";
    const button = document.createElement("button");
    button.type = "button";
    button.className = "card-button switch-action";
    button.textContent = isActive ? "当前版本" : "切换到此版本 →";
    button.disabled = isActive || busyStates.has(lastState);
    button.addEventListener("click", () => switchVersion(item.version));
    actions.append(hint, button);
    card.append(actions);
    elements.localVersions.append(card);
  });
  if (remotePackages.length) renderRemoteVersions(remotePackages);
}

async function switchVersion(version) {
  const type = elements.packageType.value;
  if (!window.confirm(`确定将 ${type} 切换到 ${version}？`)) return;
  try {
    await api("/switch", { method: "POST", body: { package: type, version } });
    toast(`${type} 已切换到 ${version}`);
    logActivity(`${type} 当前版本切换为 ${version}`);
    await Promise.all([refreshStatus(true), refreshLocalVersions(true)]);
  } catch (error) {
    toast(error.message, true);
    logActivity(`版本切换失败：${error.message}`);
  }
}

function createVersionCard(version, options) {
  const card = document.createElement("article");
  card.className = `version-card${options.active ? " is-active" : ""}`;
  const head = document.createElement("div");
  head.className = "version-card-head";
  const titleWrap = document.createElement("div");
  const tag = document.createElement("span");
  tag.className = `tag${options.active ? " active" : ""}`;
  tag.textContent = options.tag;
  const title = document.createElement("h3");
  title.textContent = version;
  titleWrap.append(tag, title);
  head.append(titleWrap);
  const metadata = document.createElement("div");
  metadata.className = "version-card-meta";
  options.metadata.forEach((text) => {
    const span = document.createElement("span");
    span.textContent = text;
    metadata.append(span);
  });
  card.append(head, metadata);
  return card;
}

function renderEmpty(container, title, detail = "") {
  container.replaceChildren();
  const card = document.createElement("div");
  card.className = "empty-card";
  const text = document.createElement("p");
  text.textContent = title;
  card.append(text);
  if (detail) {
    const small = document.createElement("small");
    small.textContent = detail;
    card.append(small);
  }
  container.append(card);
}

function setConnection(online, detail = "") {
  elements.connection.dataset.state = online ? "online" : "offline";
  elements.connectionText.textContent = online ? "本机服务在线" : "本机服务离线";
  elements.connection.title = detail;
}

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes === 0) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  const number = bytes / 1024 ** index;
  return `${number >= 100 || index === 0 ? number.toFixed(0) : number.toFixed(1)} ${units[index]}`;
}

function shortHash(hash) {
  return hash ? `SHA ${hash.slice(0, 8)}` : "SHA —";
}

function toast(message, error = false) {
  const item = document.createElement("div");
  item.className = `toast${error ? " error" : ""}`;
  item.textContent = message;
  elements.toastRegion.append(item);
  window.setTimeout(() => item.remove(), 4200);
}

function logActivity(message) {
  const item = document.createElement("li");
  const time = document.createElement("time");
  time.textContent = new Date().toLocaleTimeString("zh-CN", { hour12: false });
  const text = document.createElement("span");
  text.textContent = message;
  item.append(time, text);
  elements.activityLog.prepend(item);
  while (elements.activityLog.children.length > 30) elements.activityLog.lastElementChild.remove();
}

async function refreshAll() {
  await Promise.all([refreshStatus(true), refreshLocalVersions(true)]);
}

elements.saveSettingsButton.addEventListener("click", () => saveSettings(true));
elements.refreshRemoteButton.addEventListener("click", refreshRemoteVersions);
elements.refreshLocalButton.addEventListener("click", () => refreshLocalVersions(false));
elements.cancelButton.addEventListener("click", cancelDownload);
elements.clearLogButton.addEventListener("click", () => elements.activityLog.replaceChildren());
async function startConsole() {
  loadSettings();
  try {
    await refreshPackageCatalog();
    await refreshAll();
    connectStatusSocket();
  } catch (error) {
    setConnection(false, error.message);
    toast(error.message, true);
    logActivity(`初始化失败：${error.message}`);
  }
}

startConsole();
window.addEventListener("beforeunload", () => {
  window.clearTimeout(reconnectTimer);
  if (statusSocket) statusSocket.close();
});
