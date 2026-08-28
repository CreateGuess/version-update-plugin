# Web 控制台

这是一个不依赖 npm 或前端框架的静态页面。

```bash
python3 -m http.server 8000
```

然后访问 <http://127.0.0.1:8000>。首次打开后，在“连接参数”中填写：

- 本机插件 API：通常是
  `http://127.0.0.1/backend/plugin-http/version_update`
- 云端版本服务：例如 `http://192.168.2.100:28000`
- 设备架构及发布通道

配置保存在浏览器 `localStorage` 中，不会上传到其他服务。
