param(
    [string]$Root = "\\192.168.1.105\麦田广告\Music",
    [string[]]$Folders = @(
        "音乐",
        "我喜欢",
        "日文歌曲",
        "好听的英文歌",
        "韩语歌曲"
    ),
    [string]$SourcesOut = "$env:USERPROFILE\Desktop\net_music_sources.txt",
    [string]$ListName = "net_music.txt",
    [string]$Ffprobe = "ffprobe",
    [string[]]$Extensions = @(".mp3")
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

# net_music.txt 格式：
# 标题|原始UTF-8相对路径|格式|歌手|专辑|时长毫秒
#
# 本脚本不再对第二列执行 URL 百分号编码。
# 固件应用 round55-nas-raw-utf8-paths.patch 后，会在发送 HTTP 请求时自动编码路径。

function Normalize-TextField($Value) {
    if ($null -eq $Value) { return "" }

    $text = $Value.ToString().Trim()
    $text = [System.Text.RegularExpressions.Regex]::Replace($text, "[\r\n\t]+", " ")

    # “|”是列表字段分隔符，标签中出现时替换为全角斜杠。
    $text = $text -replace "\|", "／"
    return $text.Trim()
}

function Test-BadText($Value) {
    if ($null -eq $Value) { return $true }

    $text = $Value.ToString().Trim()
    if ([string]::IsNullOrWhiteSpace($text)) { return $true }

    # 明显解码失败符号。
    if ($text.Contains("�")) { return $true }

    # 常见 UTF-8 被错误按 Latin-1/ANSI 解码后的乱码特征。
    $badPatterns = @(
        "Ã", "Â", "Ä", "Å", "Æ", "Ç", "È", "É",
        "ã€", "ã‚", "ãƒ",
        "æ", "ç", "è", "é", "å", "ä"
    )

    foreach ($pattern in $badPatterns) {
        if ($text.Contains($pattern)) {
            return $true
        }
    }

    return $false
}

function First-GoodText($TagValue, $FallbackValue) {
    if (-not (Test-BadText $TagValue)) {
        return $TagValue.ToString().Trim()
    }

    return $FallbackValue
}

function Parse-ArtistTitle($NameWithoutExtension) {
    $artist = "未知歌手"
    $title = $NameWithoutExtension

    # 支持“歌手 - 标题”“歌手 – 标题”“歌手 — 标题”。
    $parts = $NameWithoutExtension -split "\s+[-–—]\s+", 2
    if ($parts.Count -eq 2) {
        if (-not [string]::IsNullOrWhiteSpace($parts[0])) {
            $artist = $parts[0].Trim()
        }
        if (-not [string]::IsNullOrWhiteSpace($parts[1])) {
            $title = $parts[1].Trim()
        }
    }

    return @{
        Artist = $artist
        Title = $title
    }
}

function Get-TagValue($Tags, [string[]]$Names) {
    if ($null -eq $Tags) { return "" }

    foreach ($name in $Names) {
        foreach ($property in $Tags.PSObject.Properties) {
            if ($property.Name.Equals($name, [System.StringComparison]::OrdinalIgnoreCase)) {
                if ($null -ne $property.Value) {
                    $value = $property.Value.ToString().Trim()
                    if (-not [string]::IsNullOrWhiteSpace($value)) {
                        return $value
                    }
                }
            }
        }
    }

    return ""
}

function Get-MediaInfo($FilePath) {
    $result = @{
        DurationMs = 0
        Title = ""
        Artist = ""
        Album = ""
    }

    $stdoutFile = [System.IO.Path]::GetTempFileName()
    $stderrFile = [System.IO.Path]::GetTempFileName()

    try {
        # Start-Process 直接把 ffprobe 的原始 UTF-8 输出重定向到文件，
        # 避免 Windows PowerShell 5.1 按系统代码页错误解码中文标签。
        $escapedPath = $FilePath.Replace('"', '\"')
        $argumentLine = '-v error -show_entries format=duration:format_tags=title,artist,album -of json "' + $escapedPath + '"'

        $process = Start-Process `
            -FilePath $Ffprobe `
            -ArgumentList $argumentLine `
            -NoNewWindow `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutFile `
            -RedirectStandardError $stderrFile

        if ($process.ExitCode -ne 0) {
            return $result
        }

        $jsonText = [System.IO.File]::ReadAllText(
            $stdoutFile,
            [System.Text.Encoding]::UTF8
        )

        if ([string]::IsNullOrWhiteSpace($jsonText)) {
            return $result
        }

        $info = $jsonText | ConvertFrom-Json
        if ($null -eq $info -or $null -eq $info.format) {
            return $result
        }

        if ($null -ne $info.format.duration) {
            $durationSeconds = 0.0
            $parsed = [double]::TryParse(
                $info.format.duration.ToString(),
                [System.Globalization.NumberStyles]::Float,
                [System.Globalization.CultureInfo]::InvariantCulture,
                [ref]$durationSeconds
            )

            if ($parsed -and $durationSeconds -gt 0) {
                $durationMs64 = [Math]::Round($durationSeconds * 1000.0)
                if ($durationMs64 -gt [int]::MaxValue) {
                    $durationMs64 = [int]::MaxValue
                }
                $result.DurationMs = [int]$durationMs64
            }
        }

        $tags = $info.format.tags
        $result.Title = Get-TagValue $tags @("title")
        $result.Artist = Get-TagValue $tags @("artist", "album_artist", "albumartist")
        $result.Album = Get-TagValue $tags @("album")

        return $result
    }
    catch {
        return $result
    }
    finally {
        Remove-Item -LiteralPath $stdoutFile -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrFile -Force -ErrorAction SilentlyContinue
    }
}


if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
    throw "NAS 音乐父目录不存在：$Root"
}

if ($null -eq (Get-Command $Ffprobe -ErrorAction SilentlyContinue)) {
    throw "找不到 ffprobe：$Ffprobe。请安装 FFmpeg，或使用 -Ffprobe 指定 ffprobe.exe 完整路径。"
}

$extensionSet = @{}
foreach ($extension in $Extensions) {
    if ([string]::IsNullOrWhiteSpace($extension)) { continue }

    $normalizedExtension = $extension.Trim().ToLowerInvariant()
    if (-not $normalizedExtension.StartsWith('.')) {
        $normalizedExtension = '.' + $normalizedExtension
    }
    $extensionSet[$normalizedExtension] = $true
}

if ($extensionSet.Count -eq 0) {
    throw "没有配置可扫描的音频扩展名。"
}

$utf8WithoutBom = New-Object System.Text.UTF8Encoding($false)
$sourceLines = New-Object 'System.Collections.Generic.List[string]'
$totalSongs = 0

foreach ($folder in $Folders) {
    if ([string]::IsNullOrWhiteSpace($folder)) { continue }

    $displayName = $folder.Trim()
    $sourceRelative = ($displayName -replace "\\", "/").Trim('/')
    $sourceRoot = Join-Path $Root $displayName

    if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
        Write-Warning "跳过不存在的曲库文件夹：$sourceRoot"
        continue
    }

    $rootPrefix = $sourceRoot.TrimEnd('\', '/')
    $files = @(
        Get-ChildItem -LiteralPath $sourceRoot -File -Recurse |
            Where-Object { $extensionSet.ContainsKey($_.Extension.ToLowerInvariant()) } |
            Sort-Object FullName
    )

    $items = New-Object 'System.Collections.Generic.List[string]'
    $index = 0
    $total = $files.Count

    Write-Host ""
    Write-Host "正在生成曲库：$folder（$total 首）"

    foreach ($file in $files) {
        $index++
        if (($index % 25) -eq 0 -or $index -eq $total) {
            Write-Host "  处理中：$index / $total"
        }

        if (-not $file.FullName.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            Write-Warning "跳过无法转换为相对路径的文件：$($file.FullName)"
            continue
        }

        # 每份列表的路径相对于当前曲库文件夹，而不是相对于五个文件夹的共同父目录。
        $relativePath = $file.FullName.Substring($rootPrefix.Length).TrimStart('\', '/')
        $relativePath = $relativePath -replace "\\", "/"

        $nameWithoutExtension = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
        $fileNameMetadata = Parse-ArtistTitle $nameWithoutExtension
        $mediaInfo = Get-MediaInfo $file.FullName

        $title = First-GoodText $mediaInfo.Title $fileNameMetadata.Title
        $artist = First-GoodText $mediaInfo.Artist $fileNameMetadata.Artist
        $album = First-GoodText $mediaInfo.Album "未知专辑"

        $title = Normalize-TextField $title
        $artist = Normalize-TextField $artist
        $album = Normalize-TextField $album

        if ([string]::IsNullOrWhiteSpace($title)) { $title = $nameWithoutExtension }
        if ([string]::IsNullOrWhiteSpace($artist)) { $artist = "未知歌手" }
        if ([string]::IsNullOrWhiteSpace($album)) { $album = "未知专辑" }

        $format = $file.Extension.TrimStart('.').ToLowerInvariant()
        $line = "{0}|{1}|{2}|{3}|{4}|{5}" -f `
            $title,
            $relativePath,
            $format,
            $artist,
            $album,
            $mediaInfo.DurationMs

        $items.Add($line)
    }

    # 列表直接写入对应 NAS 文件夹，Web 地址为 /music/<文件夹>/net_music.txt。
    $listPath = Join-Path $sourceRoot $ListName
    [System.IO.File]::WriteAllLines(
        $listPath,
        [string[]]$items.ToArray(),
        $utf8WithoutBom
    )

    # 固件配置格式：显示名称|相对目录|列表文件名
    $sourceLines.Add(("{0}|{1}|{2}" -f $displayName, $sourceRelative, $ListName))
    $totalSongs += $items.Count

    Write-Host "  已生成：$listPath"
    Write-Host "  歌曲数量：$($items.Count)"
}

if ($sourceLines.Count -eq 0) {
    throw "没有生成任何 NAS 曲库列表。"
}

$sourcesDirectory = Split-Path -Parent $SourcesOut
if (-not [string]::IsNullOrWhiteSpace($sourcesDirectory) -and
    -not (Test-Path -LiteralPath $sourcesDirectory)) {
    New-Item -ItemType Directory -Path $sourcesDirectory -Force | Out-Null
}

[System.IO.File]::WriteAllLines(
    $SourcesOut,
    [string[]]$sourceLines.ToArray(),
    $utf8WithoutBom
)

Write-Host ""
Write-Host "全部完成"
Write-Host "曲库数量：$($sourceLines.Count)"
Write-Host "歌曲总数：$totalSongs"
Write-Host "曲库源配置：$SourcesOut"
Write-Host "请把 net_music_sources.txt 复制到 TF 卡 /System/ 目录。"
Write-Host "设备切换曲库时只下载当前文件夹的一份 net_music.txt。"
