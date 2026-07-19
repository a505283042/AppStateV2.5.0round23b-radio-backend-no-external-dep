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
    [string[]]$Extensions = @(".mp3", ".flac"),
    [string]$CacheRoot = "$env:LOCALAPPDATA\ESP32_NAS_Multi_Library_Generator",
    [switch]$ForceProbe,
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$utf8WithoutBom = New-Object System.Text.UTF8Encoding($false)
$desktopPath = [Environment]::GetFolderPath("Desktop")
$logPath = Join-Path $desktopPath "net_music_generator.log"
$exitCode = 0

function Write-Log {
    param(
        [string]$Message,
        [ValidateSet("INFO", "WARN", "ERROR")]
        [string]$Level = "INFO"
    )

    $line = "{0} [{1}] {2}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Level, $Message
    Add-Content -LiteralPath $logPath -Value $line -Encoding UTF8

    switch ($Level) {
        "WARN"  { Write-Host $Message -ForegroundColor Yellow }
        "ERROR" { Write-Host $Message -ForegroundColor Red }
        default { Write-Host $Message }
    }
}

function Resolve-FfprobePath {
    param([string]$Requested)

    if (-not [string]::IsNullOrWhiteSpace($Requested) -and
        (Test-Path -LiteralPath $Requested -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $Requested).Path
    }

    $localFfprobe = Join-Path $PSScriptRoot "ffprobe.exe"
    if (Test-Path -LiteralPath $localFfprobe -PathType Leaf) {
        return (Resolve-Path -LiteralPath $localFfprobe).Path
    }

    foreach ($candidate in @($Requested, "ffprobe.exe", "ffprobe")) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }

        $command = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $command) {
            return $command.Source
        }
    }

    throw "找不到 ffprobe.exe。请把 ffprobe.exe 放到脚本同一目录，或使用 -Ffprobe 指定完整路径。"
}

function Normalize-TextField {
    param($Value)

    if ($null -eq $Value) { return "" }

    $text = $Value.ToString().Trim()
    $text = [System.Text.RegularExpressions.Regex]::Replace($text, "[\r\n\t]+", " ")
    $text = $text -replace "\|", "／"
    return $text.Trim()
}

function Test-BadText {
    param($Value)

    if ($null -eq $Value) { return $true }

    $text = $Value.ToString().Trim()
    if ([string]::IsNullOrWhiteSpace($text)) { return $true }
    if ($text.Contains("�")) { return $true }

    foreach ($pattern in @(
        "Ã", "Â", "Ä", "Å", "Æ", "Ç", "È", "É",
        "ã€", "ã‚", "ãƒ", "æ", "ç", "è", "é", "å", "ä"
    )) {
        if ($text.Contains($pattern)) { return $true }
    }

    return $false
}

function First-GoodText {
    param($TagValue, $FallbackValue)

    if (-not (Test-BadText $TagValue)) {
        return $TagValue.ToString().Trim()
    }

    return $FallbackValue
}

function Parse-ArtistTitle {
    param([string]$NameWithoutExtension)

    $artist = "未知歌手"
    $title = $NameWithoutExtension
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

function Get-TagValue {
    param($Tags, [string[]]$Names)

    if ($null -eq $Tags) { return "" }

    foreach ($name in $Names) {
        foreach ($property in $Tags.PSObject.Properties) {
            if ($property.Name.Equals($name, [System.StringComparison]::OrdinalIgnoreCase) -and
                $null -ne $property.Value) {
                $value = $property.Value.ToString().Trim()
                if (-not [string]::IsNullOrWhiteSpace($value)) {
                    return $value
                }
            }
        }
    }

    return ""
}

function Get-MediaInfo {
    param(
        [string]$FilePath,
        [string]$FfprobePath
    )

    $result = @{
        DurationMs = 0
        Title = ""
        Artist = ""
        Album = ""
    }

    try {
        # 直接通过 .NET 进程管道读取 JSON，避免每首歌创建两个临时文件。
        $processInfo = New-Object System.Diagnostics.ProcessStartInfo
        $processInfo.FileName = $FfprobePath
        $processInfo.Arguments = '-hide_banner -v error -show_entries format=duration:format_tags=title,artist,album,album_artist,albumartist -of json "' + $FilePath + '"'
        $processInfo.UseShellExecute = $false
        $processInfo.CreateNoWindow = $true
        $processInfo.RedirectStandardOutput = $true
        $processInfo.RedirectStandardError = $true

        try {
            $processInfo.StandardOutputEncoding = [System.Text.Encoding]::UTF8
            $processInfo.StandardErrorEncoding = [System.Text.Encoding]::UTF8
        }
        catch {
            # 较旧 .NET 没有编码属性时继续使用系统默认管道。
        }

        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $processInfo

        if (-not $process.Start()) {
            return $result
        }

        $jsonText = $process.StandardOutput.ReadToEnd()
        $stderrText = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        $processExitCode = $process.ExitCode
        $process.Dispose()

        if ($processExitCode -ne 0) {
            if (-not [string]::IsNullOrWhiteSpace($stderrText)) {
                Write-Log "ffprobe 读取失败：$FilePath；$($stderrText.Trim())" "WARN"
            }
            return $result
        }

        if ([string]::IsNullOrWhiteSpace($jsonText)) { return $result }

        $info = $jsonText | ConvertFrom-Json
        if ($null -eq $info -or $null -eq $info.format) { return $result }

        if ($null -ne $info.format.duration) {
            $durationSeconds = [double]0
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
        Write-Log "读取媒体标签异常：$FilePath；$($_.Exception.Message)" "WARN"
        return $result
    }
}

function Get-CacheFilePath {
    param(
        [string]$SourceRoot,
        [string]$CacheDirectory
    )

    if (-not (Test-Path -LiteralPath $CacheDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $CacheDirectory -Force | Out-Null
    }

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $normalizedRoot = $SourceRoot.TrimEnd([char[]]@([char]'\', [char]'/')).ToLowerInvariant()
        $hashBytes = $sha256.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($normalizedRoot))
        $hashText = ([System.BitConverter]::ToString($hashBytes)).Replace("-", "").ToLowerInvariant()
        return Join-Path $CacheDirectory ("metadata_" + $hashText.Substring(0, 24) + ".json")
    }
    finally {
        $sha256.Dispose()
    }
}

function Load-MetadataCache {
    param([string]$CachePath)

    $cacheMap = @{}
    if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) {
        return $cacheMap
    }

    try {
        $jsonText = [System.IO.File]::ReadAllText($CachePath, [System.Text.Encoding]::UTF8)
        if ([string]::IsNullOrWhiteSpace($jsonText)) {
            return $cacheMap
        }

        $document = $jsonText | ConvertFrom-Json
        foreach ($entry in @($document.Entries)) {
            if ($null -eq $entry -or [string]::IsNullOrWhiteSpace($entry.RelativePath)) {
                continue
            }

            $key = $entry.RelativePath.ToString().ToLowerInvariant()
            $cacheMap[$key] = $entry
        }
    }
    catch {
        Write-Log "缓存读取失败，将重新读取标签：$CachePath；$($_.Exception.Message)" "WARN"
        $cacheMap = @{}
    }

    return $cacheMap
}

function Save-MetadataCache {
    param(
        [string]$CachePath,
        [string]$SourceRoot,
        $Entries,
        [System.Text.Encoding]$Encoding
    )

    $document = [ordered]@{
        Version = 1
        SourceRoot = $SourceRoot
        GeneratedAtUtc = [DateTime]::UtcNow.ToString("o")
        Entries = @($Entries)
    }

    $jsonText = $document | ConvertTo-Json -Depth 6
    $tempPath = $CachePath + ".tmp"
    [System.IO.File]::WriteAllText($tempPath, $jsonText, $Encoding)
    Move-Item -LiteralPath $tempPath -Destination $CachePath -Force
}

function Write-ListIfChanged {
    param(
        [string]$ListPath,
        [string[]]$Lines,
        [System.Text.Encoding]$Encoding
    )

    $newText = [string]::Join([Environment]::NewLine, $Lines)
    if ($Lines.Count -gt 0) {
        $newText += [Environment]::NewLine
    }

    if (Test-Path -LiteralPath $ListPath -PathType Leaf) {
        try {
            $oldText = [System.IO.File]::ReadAllText($ListPath, [System.Text.Encoding]::UTF8)
            if ($oldText -eq $newText) {
                return $false
            }
        }
        catch {
            # 读取失败时继续覆盖。
        }
    }

    $tempListPath = $ListPath + ".tmp"
    [System.IO.File]::WriteAllText($tempListPath, $newText, $Encoding)

    if (Test-Path -LiteralPath $ListPath -PathType Leaf) {
        # 覆盖现有文件内容，尽量保留 NAS 上正式文件的 ACL。
        Copy-Item -LiteralPath $tempListPath -Destination $ListPath -Force
        Remove-Item -LiteralPath $tempListPath -Force -ErrorAction SilentlyContinue
    }
    else {
        Move-Item -LiteralPath $tempListPath -Destination $ListPath -Force
    }

    return $true
}

try {
    Set-Content -LiteralPath $logPath -Value ("NAS 多曲库列表生成日志 - " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss")) -Encoding UTF8

    Write-Host "=============================================" -ForegroundColor Cyan
    Write-Host " ESP32 NAS 多曲库列表生成器 v5（MP3 + FLAC 增量缓存）" -ForegroundColor Cyan
    Write-Host "=============================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Log "父目录：$Root"
    Write-Log "日志文件：$logPath"
    Write-Log "缓存目录：$CacheRoot"
    Write-Log ("强制重新读取全部标签：" + [bool]$ForceProbe)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "NAS 音乐父目录不存在或当前账号无权访问：$Root"
    }

    $resolvedFfprobe = Resolve-FfprobePath $Ffprobe
    Write-Log "ffprobe：$resolvedFfprobe"

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

    $sourceLines = New-Object 'System.Collections.Generic.List[string]'
    $totalSongs = 0
    $totalReused = 0
    $totalProbed = 0
    $totalListsWritten = 0

    foreach ($folder in $Folders) {
        if ([string]::IsNullOrWhiteSpace($folder)) { continue }

        $displayName = $folder.Trim()
        $sourceRelative = ($displayName -replace "\\", "/").Trim([char]'/')
        $sourceRoot = Join-Path $Root $displayName

        if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
            Write-Log "跳过不存在的曲库文件夹：$sourceRoot" "WARN"
            continue
        }

        try {
            $sourceRootItem = Get-Item -LiteralPath $sourceRoot -ErrorAction Stop
            $pathTrimChars = [char[]]@([char]'\', [char]'/')
            $rootPrefix = $sourceRootItem.FullName.TrimEnd($pathTrimChars)
            $cachePath = Get-CacheFilePath -SourceRoot $sourceRootItem.FullName -CacheDirectory $CacheRoot
            $oldCache = Load-MetadataCache -CachePath $cachePath

            $files = @(
                Get-ChildItem -LiteralPath $sourceRoot -File -Recurse -ErrorAction Stop |
                    Where-Object { $extensionSet.ContainsKey($_.Extension.ToLowerInvariant()) } |
                    Sort-Object FullName
            )

            $items = New-Object 'System.Collections.Generic.List[string]'
            $nextCacheEntries = New-Object 'System.Collections.Generic.List[object]'
            $index = 0
            $total = $files.Count
            $reusedCount = 0
            $probedCount = 0

            Write-Host ""
            Write-Log "正在生成曲库：$displayName（$total 首）"

            foreach ($file in $files) {
                $index++
                if (($index % 50) -eq 0 -or $index -eq $total) {
                    Write-Host "  处理中：$index / $total；缓存复用=$reusedCount；ffprobe=$probedCount"
                }

                if (-not $file.FullName.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                    Write-Log "跳过无法转换为相对路径的文件：$($file.FullName)" "WARN"
                    continue
                }

                $relativePath = $file.FullName.Substring($rootPrefix.Length).TrimStart($pathTrimChars)
                $relativePath = $relativePath -replace "\\", "/"
                $cacheKey = $relativePath.ToLowerInvariant()
                $cachedEntry = $oldCache[$cacheKey]
                $mediaInfo = $null

                $cacheValid = $false
                if (-not $ForceProbe -and $null -ne $cachedEntry) {
                    try {
                        $cacheValid = (
                            [int64]$cachedEntry.Length -eq [int64]$file.Length -and
                            [int64]$cachedEntry.LastWriteTimeUtcTicks -eq [int64]$file.LastWriteTimeUtc.Ticks
                        )
                    }
                    catch {
                        $cacheValid = $false
                    }
                }

                if ($cacheValid) {
                    $mediaInfo = @{
                        DurationMs = [int]$cachedEntry.DurationMs
                        Title = $cachedEntry.Title
                        Artist = $cachedEntry.Artist
                        Album = $cachedEntry.Album
                    }
                    $reusedCount++
                }
                else {
                    $mediaInfo = Get-MediaInfo -FilePath $file.FullName -FfprobePath $resolvedFfprobe
                    $probedCount++
                }

                $nameWithoutExtension = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
                $fileNameMetadata = Parse-ArtistTitle $nameWithoutExtension

                $title = Normalize-TextField (First-GoodText $mediaInfo.Title $fileNameMetadata.Title)
                $artist = Normalize-TextField (First-GoodText $mediaInfo.Artist $fileNameMetadata.Artist)
                $album = Normalize-TextField (First-GoodText $mediaInfo.Album "未知专辑")

                if ([string]::IsNullOrWhiteSpace($title)) { $title = $nameWithoutExtension }
                if ([string]::IsNullOrWhiteSpace($artist)) { $artist = "未知歌手" }
                if ([string]::IsNullOrWhiteSpace($album)) { $album = "未知专辑" }

                $format = $file.Extension.TrimStart('.').ToLowerInvariant()
                $durationMs = [int]$mediaInfo.DurationMs
                $line = "{0}|{1}|{2}|{3}|{4}|{5}" -f `
                    $title,
                    $relativePath,
                    $format,
                    $artist,
                    $album,
                    $durationMs

                $items.Add($line)
                $nextCacheEntries.Add([pscustomobject][ordered]@{
                    RelativePath = $relativePath
                    Length = [int64]$file.Length
                    LastWriteTimeUtcTicks = [int64]$file.LastWriteTimeUtc.Ticks
                    Title = $title
                    Artist = $artist
                    Album = $album
                    DurationMs = $durationMs
                })
            }

            $listPath = Join-Path $sourceRoot $ListName
            $listWritten = Write-ListIfChanged `
                -ListPath $listPath `
                -Lines ([string[]]$items.ToArray()) `
                -Encoding $utf8WithoutBom

            Save-MetadataCache `
                -CachePath $cachePath `
                -SourceRoot $sourceRootItem.FullName `
                -Entries $nextCacheEntries.ToArray() `
                -Encoding $utf8WithoutBom

            $sourceLines.Add(("{0}|{1}|{2}" -f $displayName, $sourceRelative, $ListName))
            $totalSongs += $items.Count
            $totalReused += $reusedCount
            $totalProbed += $probedCount
            if ($listWritten) { $totalListsWritten++ }

            Write-Log (
                "已完成：{0}；歌曲={1}；缓存复用={2}；ffprobe={3}；列表写入={4}" -f `
                    $displayName,
                    $items.Count,
                    $reusedCount,
                    $probedCount,
                    $listWritten
            )
        }
        catch {
            Write-Log "曲库生成失败：$displayName；$($_.Exception.Message)" "ERROR"
            continue
        }
    }

    if ($sourceLines.Count -eq 0) {
        throw "没有成功生成任何 NAS 曲库列表。请检查文件夹名称、NAS 权限和日志。"
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
    Write-Host "全部完成" -ForegroundColor Green
    Write-Log "曲库数量：$($sourceLines.Count)"
    Write-Log "歌曲总数：$totalSongs"
    Write-Log "缓存复用：$totalReused"
    Write-Log "实际执行 ffprobe：$totalProbed"
    Write-Log "实际更新列表文件：$totalListsWritten"
    Write-Log "曲库源配置：$SourcesOut"
    Write-Host ""
    Write-Host "缓存复用：$totalReused；实际 ffprobe：$totalProbed" -ForegroundColor Cyan
    Write-Host "只有新增或已修改歌曲会重新读取标签。" -ForegroundColor Cyan
    Write-Host "请把 net_music_sources.txt 复制到 TF 卡 /System/config/ 目录。" -ForegroundColor Cyan
}
catch {
    $exitCode = 1
    Write-Host ""
    Write-Log ("执行失败：" + $_.Exception.Message) "ERROR"
    Write-Host "请查看桌面日志：$logPath" -ForegroundColor Yellow
}
finally {
    if (-not $NoPause) {
        Write-Host ""
        [void](Read-Host "按 Enter 键关闭窗口")
    }
}

exit $exitCode
