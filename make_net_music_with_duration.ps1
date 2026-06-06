[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$root = "\\192.168.1.105\麦田广告\Music\音乐"
$out = "$env:USERPROFILE\Desktop\net_music.txt"
$ffprobe = "ffprobe"

function Escape-Field($s) {
    if ($null -eq $s) { return "" }
    return ($s.ToString().Trim() -replace "\|", "／")
}

function First-NonEmpty($a, $b) {
    if ($null -ne $a -and -not [string]::IsNullOrWhiteSpace($a.ToString())) {
        return $a.ToString().Trim()
    }
    return $b
}

function Test-BadText($s) {
    if ($null -eq $s) { return $true }

    $t = $s.ToString().Trim()
    if ([string]::IsNullOrWhiteSpace($t)) { return $true }

    # 明显解码失败符号
    if ($t.Contains("�")) { return $true }

    # 常见 UTF-8 被错误按 Latin-1/ANSI 解码后的乱码特征
    $badPatterns = @(
        "Ã", "Â", "Ä", "Å", "Æ", "Ç", "È", "É",
        "ã€", "ã‚", "ãƒ",
        "æ", "ç", "è", "é", "å", "ä"
    )

    foreach ($p in $badPatterns) {
        if ($t.Contains($p)) {
            return $true
        }
    }

    return $false
}

function First-GoodText($tagValue, $fallbackValue) {
    if (-not (Test-BadText $tagValue)) {
        return $tagValue.ToString().Trim()
    }

    return $fallbackValue
}

function Parse-ArtistTitle($nameWithoutExt) {
    $artist = "NAS"
    $title = $nameWithoutExt

    $parts = $nameWithoutExt -split "\s+-\s+", 2
    if ($parts.Count -eq 2) {
        $artist = $parts[0].Trim()
        $title = $parts[1].Trim()
    }

    return @{
        Artist = $artist
        Title = $title
    }
}

function Get-MediaInfo($filePath) {
    $result = @{
        DurationMs = 0
        Title = ""
        Artist = ""
        Album = ""
    }

    try {
        $probeArgs = @(
            "-v", "error",
            "-show_entries", "format=duration:format_tags=title,artist,album",
            "-of", "json",
            $filePath
        )

        $jsonText = & $ffprobe @probeArgs 2>$null

        if ([string]::IsNullOrWhiteSpace($jsonText)) {
            return $result
        }

        $info = $jsonText | ConvertFrom-Json

        if ($null -ne $info.format.duration) {
            $value = 0.0
            if ([double]::TryParse($info.format.duration.ToString(), [ref]$value)) {
                $result.DurationMs = [int][Math]::Round($value * 1000)
            }
        }

        if ($null -ne $info.format.tags) {
            if ($null -ne $info.format.tags.title) {
                $result.Title = $info.format.tags.title.ToString().Trim()
            }

            if ($null -ne $info.format.tags.artist) {
                $result.Artist = $info.format.tags.artist.ToString().Trim()
            }

            if ($null -ne $info.format.tags.album) {
                $result.Album = $info.format.tags.album.ToString().Trim()
            }
        }

        return $result
    } catch {
        return $result
    }
}

$files = Get-ChildItem $root -File -Recurse -Include *.mp3 | Sort-Object FullName
$total = $files.Count
$index = 0

$items = foreach ($file in $files) {
    $index++

    if (($index % 50) -eq 0) {
        Write-Host "Processing $index / $total"
    }

    $relative = $file.FullName.Substring($root.Length).TrimStart('\')
    $relativeUrl = $relative -replace "\\", "/"

    $encodedParts = $relativeUrl.Split("/") | ForEach-Object {
        [System.Uri]::EscapeDataString($_)
    }
    $encodedPath = $encodedParts -join "/"

    $name = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
    $fileNameMeta = Parse-ArtistTitle $name
    $mediaInfo = Get-MediaInfo $file.FullName

    $title = First-GoodText $mediaInfo.Title $fileNameMeta.Title
    $artist = First-GoodText $mediaInfo.Artist $fileNameMeta.Artist
    $album = First-GoodText $mediaInfo.Album "NAS"
    $durationMs = $mediaInfo.DurationMs

    $title = Escape-Field $title
    $artist = Escape-Field $artist
    $album = Escape-Field $album

    "{0}|{1}|mp3|{2}|{3}|{4}" -f $title, $encodedPath, $artist, $album, $durationMs
}

$items | Set-Content -Encoding UTF8 $out

Write-Host "Generated: $out"
Write-Host "Count: $($items.Count)"
