# ESP32-S3 曲库电脑端扫描器

本程序在 Windows 电脑上直接扫描 TF 卡的 `Music` 目录，并生成与当前设备固件兼容的：

- `System/music_index_v3.bin`
- `System/music_manifest_v1.bin`

同时生成便于核对的：

- `System/music_scan_report.json`
- `System/music_scan_tracks.csv`

## 功能

- 图形界面选择 TF 卡目录。
- 支持 MP3 和 FLAC。
- 读取 MP3 ID3v2.3、ID3v2.4、ID3v1 标签。
- 读取 FLAC Vorbis Comment。
- 记录 MP3 APIC 和 FLAC PICTURE 内嵌封面的文件偏移与大小。
- 自动寻找同名 `.lrc` 歌词。
- 自动寻找 `cover`、`folder`、`front` 等目录封面。
- 快速增量扫描：优先比较文件大小和 FAT 修改时间，未变化音频不读取内容 CRC。
- 严格增量扫描：逐文件校验内容 CRC，适合怀疑时间戳不可靠时使用。
- 强制全量扫描：重新解析所有歌曲。
- 检测新增、修改、删除歌曲。
- 小文件使用完整 CRC；大文件使用大小与头、中、尾 CRC。
- 索引和 Manifest 使用 CRC 校验及 `.tmp`、`.bak` 原子替换。
- 可单独校验现有索引与 Manifest 是否配对。

## 运行环境

- Windows 10 或 Windows 11。
- Python 3.10 或更高版本。
- 运行扫描器不需要安装任何第三方 Python 库。

## 图形界面运行

1. 安装 Python 3，并在安装界面勾选 `Add Python to PATH`。
2. 双击 `启动曲库扫描器.bat`。
3. 选择 TF 卡根目录，例如：

   ```text
   E:\
   ```

   也可以直接选择：

   ```text
   E:\Music
   ```

4. 点击：

   - `快速增量扫描`：日常使用；音频优先比较大小和 FAT 时间，歌词与目录封面仍校验内容 CRC。
   - `严格增量扫描`：逐文件校验音频内容 CRC，速度较慢但检测更严格。
   - `强制全量扫描`：忽略旧 Manifest，重新解析全部歌曲。
   - `校验现有索引`：只检查二进制索引、Manifest、CRC 和路径集合。

5. 扫描完成后安全弹出 TF 卡，再插回播放器。

## 命令行运行

快速增量扫描：

```bat
py -3 music_library_scanner.py --root E:\
```

严格增量扫描：

```bat
py -3 music_library_scanner.py --root E:\ --strict
```

强制全量扫描：

```bat
py -3 music_library_scanner.py --root E:\ --full
```

只校验索引：

```bat
py -3 music_library_scanner.py --root E:\ --verify
```

## 制作单文件 EXE

双击：

```text
制作Windows_EXE.bat
```

脚本会安装 PyInstaller，然后生成：

```text
dist\ESP32_Music_Library_Scanner.exe
```

制作 EXE 需要联网安装 PyInstaller；普通运行扫描器不需要联网。

## TF 卡目录结构

```text
TF卡根目录
├─ Music
│  ├─ 歌手
│  │  └─ 专辑
│  │     ├─ 01 - 歌曲.mp3
│  │     ├─ 01 - 歌曲.lrc
│  │     └─ cover.jpg
│  └─ ...
└─ System
   ├─ music_index_v3.bin
   ├─ music_manifest_v1.bin
   ├─ music_scan_report.json
   └─ music_scan_tracks.csv
```

目录层级仅作为无标签歌曲的兜底：

- 多层目录：第一层作为歌手，最后一层作为专辑。
- 单层目录：目录名作为专辑。
- 音频标签存在时，以音频标签为准。

## 首次运行

以下情况会自动执行全量扫描：

- 没有旧索引或 Manifest。
- 旧文件 CRC 错误。
- 旧 Manifest 与 Catalog CRC 不匹配。
- 索引与 Manifest 曲目路径集合不一致。
- 旧格式 Manifest 没有 Catalog CRC。

全量扫描完成后，下一次即可使用快速或严格增量扫描。

## 兼容性范围

当前电脑端程序与固件中的 V3 / Manifest v2 格式保持一致：

- Index magic：`MIDX`
- Index version：`3`
- Manifest magic：`MNF1`
- Manifest version：`2`（仍可读取 version 1，并在下一次扫描后自动升级）
- TrackRowV3：44 字节
- ArtistRowV3：4 字节
- AlbumRowV3：12 字节

当前只扫描：

- `.mp3`
- `.flac`

不会扫描 AAC、WAV、APE、M4A 或网络曲库。

## 安全说明

保存时程序先写入 `.tmp` 并完成 CRC 校验，再将原文件保留为 `.bak`。不要在写入过程中拔出 TF 卡。扫描完成后应通过 Windows 的“安全删除硬件”弹出 TF 卡。

## v1.2.0 兼容性

- 输出设备端兼容的 Manifest v3。
- Manifest v3 包含目录 FAT 时间、继承封面和子树曲目数快照。
- 可读取旧版 Manifest v1/v2；下一次成功扫描会自动升级为 v3。
- 电脑端当前仍提供快速、严格和全量扫描；设备端的“超快速目录重扫”可直接使用电脑生成的 v3 清单。
