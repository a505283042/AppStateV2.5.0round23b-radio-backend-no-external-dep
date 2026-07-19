# NAS 多曲库列表生成

1. NAS Web 根目录映射到共同父目录：`/volume1/麦田广告/Music -> /volume1/web/music`。
2. 运行 `make_net_music_multi_sources_raw_utf8.ps1`。
3. 脚本会在五个 NAS 子目录中分别生成一份 `net_music.txt`。
4. 把桌面生成的 `net_music_sources.txt` 复制到 TF 卡 `/System/`。
5. TF 卡 `/System/net_music_base.txt` 保持：`http://192.168.1.105:8080/music/`。

设备只下载当前选中曲库的列表，切换时先释放旧列表。
