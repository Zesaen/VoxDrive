# VoxDrive 启动/关闭速查

以下命令均在 **Jetson** 上执行（`ssh nvidia` 登录或板端终端）。

## 启动

```bash
~/Desktop/VoxDrive/jetson/scripts/start_all.sh --ui    # 含 dashboard 界面（Jetson 屏幕，自动开预览）
~/Desktop/VoxDrive/jetson/scripts/start_all.sh         # 纯后台服务（无界面）
```

`--ui` 在板端桌面终端或 SSH 里都能用（SSH 时自动 `DISPLAY=:0` 显示到 Jetson 屏幕）。

服务已在跑、只想补开界面：

```bash
DISPLAY=:0 VOX_DASH_AUTOPREVIEW=1 nohup python3 ~/Desktop/VoxDrive/jetson/dashboard/dashboard_ui.py >/tmp/dashboard.log 2>&1 &
```

## 键盘语音测试

```bash
python3 ~/Desktop/VoxDrive/jetson/services/asr/stdin_asr.py
```

逐行输入（Ctrl+C 退出）：

```
现在录着吗
行车记录仪还剩多少存储
帮我拍张照
停止录像 / 开始录像
打开预览 / 关闭预览
```

浏览器看画面（同网段）：http://192.168.137.190:8888/live/dashcam

## 停止 / 关机

```bash
~/Desktop/VoxDrive/jetson/scripts/stop_all.sh        # 停双板服务
~/Desktop/VoxDrive/jetson/scripts/shutdown_all.sh    # 双板关机（本机输一次 sudo 密码）
```
