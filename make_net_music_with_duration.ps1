$root = "\\192.168.1.105\麦田广告\Music\音乐"
$out = "$env:USERPROFILE\Desktop\net_music.txt"
$ffprobe = "ffprobe"

function Escape-Field($s) {
    if ($null -eq $s) { return "" }
    return ($s.ToString().Trim() -replace "\|", "／")
}

function Get-DurationMs($filePath) {
    try {
        $probeArgs = @(
            "-v", "error",
            "-show_entries", "format=duration",
            "-of", "default=noprint_wrappers=1:nokey=1",
            $filePath
        )

        $seconds = & $ffprobe @probeArgs 2>$null

        if ([string]::IsNullOrWhiteSpace($seconds)) {
            return 0
        }

        $value = 0.0
        if ([double]::TryParse($seconds.Trim(), [ref]$value)) {
            return [int][Math]::Round($value * 1000)
        }

        return 0
    } catch {
        return 0
    }
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
    $meta = Parse-ArtistTitle $name

    $title = Escape-Field $meta.Title
    $artist = Escape-Field $meta.Artist
    $album = "NAS"
    $durationMs = Get-DurationMs $file.FullName

    "{0}|{1}|mp3|{2}|{3}|{4}" -f $title, $encodedPath, $artist, $album, $durationMs
}

$items | Set-Content -Encoding UTF8 $out

Write-Host "Generated: $out"
Write-Host "Count: $($items.Count)"
