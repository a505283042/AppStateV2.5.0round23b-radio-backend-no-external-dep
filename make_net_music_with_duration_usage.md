# make_net_music_with_duration.ps1 使用说明

## 功能说明

本脚本用于扫描 NAS 网络共享目录中的 MP3 文件，提取元数据（标题、艺术家、专辑、时长）并生成 `net_music.txt` 文件，供 ESP32 音乐播放器读取使用。

## 前置依赖

1. **ffprobe** - 需安装并配置到系统 PATH 中
   - 下载地址：https://ffmpeg.org/download.html
   - 安装后确保 `ffprobe --version` 可正常执行

2. **PowerShell** - Windows 自带，需支持 PowerShell 5.1 或更高版本

## 配置修改

编辑脚本开头的配置参数：

```powershell
$root = "\\192.168.1.105\麦田广告\Music\音乐"  # NAS 共享目录路径
$out = "$env:USERPROFILE\Desktop\net_music.txt"  # 输出文件路径
$ffprobe = "ffprobe"  # ffprobe 可执行文件路径（若不在 PATH 中需指定完整路径）
```

## 执行命令

### 方式一：直接运行（推荐）

```powershell
powershell -ExecutionPolicy Bypass -File "C:\Users\YCB\Desktop\AppStateV2.5.0round23b-radio-backend-no-external-dep\make_net_music_with_duration.ps1"
```

### 方式二：PowerShell 终端运行

```powershell
cd "C:\Users\YCB\Desktop\AppStateV2.5.0round23b-radio-backend-no-external-dep"
.\make_net_music_with_duration.ps1
```

## 输出格式

生成的 `net_music.txt` 文件每行格式如下：

```text
标题|URL编码路径|格式|艺术家|专辑|时长(毫秒)
```

示例：
```text
夜曲|Jay%20Chou/Ye%20Qu.mp3|mp3|周杰伦|范特西|245000
```

## 特性说明

1. **乱码自动检测** - ID3 标签解码失败时自动回退到文件名解析
2. **URL 编码** - 自动处理中文和特殊字符的 URL 编码
3. **进度显示** - 每处理 50 个文件显示一次进度
4. **管道符转义** - 标题中的 `|` 会被替换为 `／`

## 使用步骤

1. 确保 NAS 共享目录可正常访问
2. 修改脚本中的 `$root` 为实际的 NAS 共享路径
3. 执行脚本生成 `net_music.txt`
4. 将生成的 `net_music.txt` 复制到 TF 卡的 `System/config/` 目录
5. 重启 ESP32 音乐播放器

## 注意事项

- 首次运行可能需要等待较长时间，取决于音乐文件数量
- 确保运行脚本的用户对 NAS 共享目录有读取权限
- 若 `ffprobe` 不在系统 PATH 中，需指定完整路径，如：
  ```powershell
  $ffprobe = "C:\ffmpeg\bin\ffprobe.exe"
  ```