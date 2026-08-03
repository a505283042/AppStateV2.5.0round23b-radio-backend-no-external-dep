# ESP32 NAS Music Scanner UI R56 - ASCII source, Windows PowerShell 5.1 compatible
param(
    [string]$Root = "",
    [string[]]$Folders = @(),
    [string]$SourcesOut = "$env:USERPROFILE\Desktop\net_music_sources.txt",
    [string]$ListName = "net_music.txt",
    [string]$Ffprobe = "ffprobe",
    [string[]]$Extensions = @(".mp3", ".flac"),
    [string]$CacheRoot = "$env:LOCALAPPDATA\ESP32_NAS_Multi_Library_Generator",
    [switch]$ForceProbe,
    [switch]$ForceRescan,
    [switch]$NoUi,
    [switch]$NoPause
)

$ScannerVersion = "R56"
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$utf8WithoutBom = New-Object System.Text.UTF8Encoding($false)
$desktopPath = [Environment]::GetFolderPath("Desktop")
$logPath = Join-Path $desktopPath "net_music_generator.log"
$settingsPath = Join-Path $CacheRoot "ui_settings.json"
$exitCode = 0
Write-Host ("ESP32 NAS Music Scanner UI {0}" -f $ScannerVersion)
$uiWasUsed = -not [bool]$NoUi

function Decode-Utf8 {
    param([string]$Base64)
    return [System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String($Base64))
}

function Normalize-RootPath {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }

    $normalized = $Value.Trim()
    if ($normalized.Length -ge 2) {
        $first = $normalized[0]
        $last = $normalized[$normalized.Length - 1]
        if (($first -eq [char]34 -and $last -eq [char]34) -or
            ($first -eq [char]39 -and $last -eq [char]39)) {
            $normalized = $normalized.Substring(1, $normalized.Length - 2).Trim()
        }
    }

    $normalized = $normalized.Replace([char]'/', [char]'\')

    $isDrivePath = (
        $normalized.Length -ge 3 -and
        [char]::IsLetter($normalized[0]) -and
        $normalized[1] -eq [char]':' -and
        $normalized[2] -eq [char]'\'
    )

    if (-not $isDrivePath) {
        if ($normalized.StartsWith('\\', [System.StringComparison]::Ordinal)) {
            # Already a complete UNC path.
        }
        elseif ($normalized.StartsWith('\', [System.StringComparison]::Ordinal)) {
            # A single leading slash was supplied. Promote it to UNC.
            $normalized = '\' + $normalized
        }
        elseif ($normalized.Contains('\')) {
            # Allow users to enter server\share\folder without the leading UNC slashes.
            $normalized = '\\' + $normalized.TrimStart([char]'\')
        }
    }

    $isDriveRoot = (
        $normalized.Length -eq 3 -and
        [char]::IsLetter($normalized[0]) -and
        $normalized[1] -eq [char]':' -and
        $normalized[2] -eq [char]'\'
    )
    if (-not $isDriveRoot) {
        $normalized = $normalized.TrimEnd([char]'\')
    }

    return $normalized
}

$Ui = @{
    Title = Decode-Utf8 "RVNQMzIgTkFTIOWkmuabsuW6k+WIl+ihqOeUn+aIkOWZqA=="
    RootLabel = Decode-Utf8 "TkFTIOmfs+S5kOagueebruW9le+8mg=="
    Browse = Decode-Utf8 "5rWP6KeILi4u"
    Refresh = Decode-Utf8 "6K+G5Yir5LiA57qn55uu5b2V"
    FoldersLabel = Decode-Utf8 "6YCJ5oup6KaB55Sf5oiQ5puy5bqT55qE5LiA57qn55uu5b2V77ya"
    SelectAll = Decode-Utf8 "5YWo6YCJ"
    ClearAll = Decode-Utf8 "5YWo5LiN6YCJ"
    ModeGroup = Decode-Utf8 "5omr5o+P5qih5byP"
    Incremental = Decode-Utf8 "5aKe6YeP5omr5o+P77yI5aSN55So5pyq5Y+Y5YyW5paH5Lu255qE5pyJ5pWI57yT5a2Y77yJ"
    Force = Decode-Utf8 "5by65Yi26YeN5omr77yI5omA5pyJ6Z+z6aKR6YeN5paw5omn6KGMIGZmcHJvYmXvvIk="
    OutputLabel = Decode-Utf8 "5puy5bqT5rqQ6YWN572u6L6T5Ye677ya"
    Start = Decode-Utf8 "5byA5aeL55Sf5oiQ"
    Cancel = Decode-Utf8 "5Y+W5raI"
    RootMissing = Decode-Utf8 "6K+36L6T5YWl5pyJ5pWI55qEIE5BUyDpn7PkuZDmoLnnm67lvZXjgII="
    RootAccess = Decode-Utf8 "55uu5b2V5LiN5a2Y5Zyo44CB5peg5rOV6K6/6Zeu77yM5oiW5b2T5YmN6LSm5Y+35rKh5pyJ5p2D6ZmQ77ya"
    NoFolders = Decode-Utf8 "6K+l5qC555uu5b2V5LiL5rKh5pyJ5om+5Yiw5LiA57qn5a2Q55uu5b2V44CC"
    SelectFolder = Decode-Utf8 "6K+36Iez5bCR6YCJ5oup5LiA5Liq5LiA57qn55uu5b2V44CC"
    DetectFailed = Decode-Utf8 "6K+75Y+W5LiA57qn55uu5b2V5aSx6LSl77ya"
    ErrorTitle = Decode-Utf8 "6ZSZ6K+v"
    InfoTitle = Decode-Utf8 "5o+Q56S6"
    DoneTitle = Decode-Utf8 "55Sf5oiQ5a6M5oiQ"
    Done = Decode-Utf8 "TkFTIOabsuW6k+WIl+ihqOeUn+aIkOWujOaIkOOAgg=="
    Failed = Decode-Utf8 "55Sf5oiQ5aSx6LSl77ya"
    OpenLog = Decode-Utf8 "5pel5b+X5paH5Lu277ya"
    SavedRoot = Decode-Utf8 "5LiK5qyh55uu5b2V77ya"
    StatusDetected = Decode-Utf8 "5bey6K+G5YirIHswfSDkuKrkuIDnuqfnm67lvZXjgII="
    StatusReady = Decode-Utf8 "6K+36YCJ5oup55uu5b2V5ZKM5omr5o+P5qih5byP44CC"
    UnknownArtist = Decode-Utf8 "5pyq55+l5q2M5omL"
    UnknownAlbum = Decode-Utf8 "5pyq55+l5LiT6L6R"
    UiNote = Decode-Utf8 "5qC555uu5b2V5Y+v6L6T5YWlIDE5Mi4xNjguMS4xMDVc5YWx5LqrXE11c2lj77yM5byA5aS055qEIFxcIOWPr+ecgeeVpe+8m+mAieaLqeaIlui+k+WFpeWQjueCueWHu+KAnOivhuWIq+S4gOe6p+ebruW9leKAneOAgg=="
}

$unknownArtist = $Ui.UnknownArtist
$unknownAlbum = $Ui.UnknownAlbum
$safePipeReplacement = [string][char]0xFF0F

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

function Ensure-Directory {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Load-UiSettings {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }

    try {
        $jsonText = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
        if ([string]::IsNullOrWhiteSpace($jsonText)) {
            return $null
        }
        return ($jsonText | ConvertFrom-Json)
    }
    catch {
        return $null
    }
}

function Save-UiSettings {
    param(
        [string]$Path,
        [string]$SelectedRoot,
        [string[]]$SelectedFolders,
        [bool]$SelectedForceRescan
    )

    try {
        $directory = Split-Path -Parent $Path
        if (-not [string]::IsNullOrWhiteSpace($directory)) {
            Ensure-Directory $directory
        }

        $document = [ordered]@{
            Version = 1
            Root = $SelectedRoot
            Folders = @($SelectedFolders)
            ForceRescan = $SelectedForceRescan
        }

        $jsonText = $document | ConvertTo-Json -Depth 4
        [System.IO.File]::WriteAllText($Path, $jsonText, $utf8WithoutBom)
    }
    catch {
        # UI settings are optional. Scan can continue if saving fails.
    }
}

function Get-FirstLevelFolders {
    param([string]$SelectedRoot)

    if ([string]::IsNullOrWhiteSpace($SelectedRoot)) {
        throw $Ui.RootMissing
    }

    $trimmedRoot = Normalize-RootPath $SelectedRoot
    if (-not (Test-Path -LiteralPath $trimmedRoot -PathType Container)) {
        throw ($Ui.RootAccess + $trimmedRoot)
    }

    return @(
        Get-ChildItem -LiteralPath $trimmedRoot -Directory -ErrorAction Stop |
            Where-Object { -not ($_.Attributes -band [System.IO.FileAttributes]::Hidden) } |
            Sort-Object Name |
            ForEach-Object { $_.Name }
    )
}

function Show-SetupDialog {
    param(
        [string]$InitialRoot,
        [string[]]$InitialFolders,
        [bool]$InitialForceRescan,
        [string]$OutputPath
    )

    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing

    [System.Windows.Forms.Application]::EnableVisualStyles()

    $form = New-Object System.Windows.Forms.Form
    $form.Text = $Ui.Title + " [" + $ScannerVersion + "]"
    $form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
    $form.Size = New-Object System.Drawing.Size(760, 610)
    $form.MinimumSize = New-Object System.Drawing.Size(700, 560)
    $form.Font = New-Object System.Drawing.Font("Microsoft YaHei UI", 9)
    $form.MaximizeBox = $false

    $rootLabel = New-Object System.Windows.Forms.Label
    $rootLabel.Text = $Ui.RootLabel
    $rootLabel.Location = New-Object System.Drawing.Point(18, 18)
    $rootLabel.AutoSize = $true

    $rootBox = New-Object System.Windows.Forms.TextBox
    $rootBox.Location = New-Object System.Drawing.Point(18, 42)
    $rootBox.Size = New-Object System.Drawing.Size(540, 28)
    $rootBox.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Top -bor
        [System.Windows.Forms.AnchorStyles]::Left -bor
        [System.Windows.Forms.AnchorStyles]::Right
    )
    $rootBox.Text = $InitialRoot

    $browseButton = New-Object System.Windows.Forms.Button
    $browseButton.Text = $Ui.Browse
    $browseButton.Location = New-Object System.Drawing.Point(570, 40)
    $browseButton.Size = New-Object System.Drawing.Size(75, 30)
    $browseButton.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Top -bor
        [System.Windows.Forms.AnchorStyles]::Right
    )

    $refreshButton = New-Object System.Windows.Forms.Button
    $refreshButton.Text = $Ui.Refresh
    $refreshButton.Location = New-Object System.Drawing.Point(650, 40)
    $refreshButton.Size = New-Object System.Drawing.Size(90, 30)
    $refreshButton.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Top -bor
        [System.Windows.Forms.AnchorStyles]::Right
    )

    $noteLabel = New-Object System.Windows.Forms.Label
    $noteLabel.Text = $Ui.UiNote
    $noteLabel.Location = New-Object System.Drawing.Point(18, 76)
    $noteLabel.Size = New-Object System.Drawing.Size(720, 24)
    $noteLabel.ForeColor = [System.Drawing.Color]::DimGray
    $noteLabel.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Top -bor
        [System.Windows.Forms.AnchorStyles]::Left -bor
        [System.Windows.Forms.AnchorStyles]::Right
    )

    $foldersLabel = New-Object System.Windows.Forms.Label
    $foldersLabel.Text = $Ui.FoldersLabel
    $foldersLabel.Location = New-Object System.Drawing.Point(18, 108)
    $foldersLabel.AutoSize = $true

    $folderList = New-Object System.Windows.Forms.CheckedListBox
    $folderList.Location = New-Object System.Drawing.Point(18, 132)
    $folderList.Size = New-Object System.Drawing.Size(722, 248)
    $folderList.CheckOnClick = $true
    $folderList.HorizontalScrollbar = $true
    $folderList.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Top -bor
        [System.Windows.Forms.AnchorStyles]::Bottom -bor
        [System.Windows.Forms.AnchorStyles]::Left -bor
        [System.Windows.Forms.AnchorStyles]::Right
    )

    $selectAllButton = New-Object System.Windows.Forms.Button
    $selectAllButton.Text = $Ui.SelectAll
    $selectAllButton.Location = New-Object System.Drawing.Point(18, 388)
    $selectAllButton.Size = New-Object System.Drawing.Size(80, 28)
    $selectAllButton.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Bottom -bor
        [System.Windows.Forms.AnchorStyles]::Left
    )

    $clearAllButton = New-Object System.Windows.Forms.Button
    $clearAllButton.Text = $Ui.ClearAll
    $clearAllButton.Location = New-Object System.Drawing.Point(104, 388)
    $clearAllButton.Size = New-Object System.Drawing.Size(80, 28)
    $clearAllButton.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Bottom -bor
        [System.Windows.Forms.AnchorStyles]::Left
    )

    $modeGroup = New-Object System.Windows.Forms.GroupBox
    $modeGroup.Text = $Ui.ModeGroup
    $modeGroup.Location = New-Object System.Drawing.Point(18, 425)
    $modeGroup.Size = New-Object System.Drawing.Size(722, 75)
    $modeGroup.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Bottom -bor
        [System.Windows.Forms.AnchorStyles]::Left -bor
        [System.Windows.Forms.AnchorStyles]::Right
    )

    $incrementalRadio = New-Object System.Windows.Forms.RadioButton
    $incrementalRadio.Text = $Ui.Incremental
    $incrementalRadio.Location = New-Object System.Drawing.Point(14, 21)
    $incrementalRadio.Size = New-Object System.Drawing.Size(680, 22)
    $incrementalRadio.Checked = -not $InitialForceRescan

    $forceRadio = New-Object System.Windows.Forms.RadioButton
    $forceRadio.Text = $Ui.Force
    $forceRadio.Location = New-Object System.Drawing.Point(14, 45)
    $forceRadio.Size = New-Object System.Drawing.Size(680, 22)
    $forceRadio.Checked = $InitialForceRescan

    $modeGroup.Controls.AddRange(@($incrementalRadio, $forceRadio))

    $outputLabel = New-Object System.Windows.Forms.Label
    $outputLabel.Text = $Ui.OutputLabel + " " + $OutputPath
    $outputLabel.Location = New-Object System.Drawing.Point(18, 508)
    $outputLabel.Size = New-Object System.Drawing.Size(520, 24)
    $outputLabel.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Bottom -bor
        [System.Windows.Forms.AnchorStyles]::Left -bor
        [System.Windows.Forms.AnchorStyles]::Right
    )
    $outputLabel.ForeColor = [System.Drawing.Color]::DimGray

    $statusLabel = New-Object System.Windows.Forms.Label
    $statusLabel.Text = $Ui.StatusReady
    $statusLabel.Location = New-Object System.Drawing.Point(18, 538)
    $statusLabel.Size = New-Object System.Drawing.Size(430, 24)
    $statusLabel.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Bottom -bor
        [System.Windows.Forms.AnchorStyles]::Left -bor
        [System.Windows.Forms.AnchorStyles]::Right
    )

    $startButton = New-Object System.Windows.Forms.Button
    $startButton.Text = $Ui.Start
    $startButton.Location = New-Object System.Drawing.Point(560, 530)
    $startButton.Size = New-Object System.Drawing.Size(90, 32)
    $startButton.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Bottom -bor
        [System.Windows.Forms.AnchorStyles]::Right
    )

    $cancelButton = New-Object System.Windows.Forms.Button
    $cancelButton.Text = $Ui.Cancel
    $cancelButton.Location = New-Object System.Drawing.Point(650, 530)
    $cancelButton.Size = New-Object System.Drawing.Size(90, 32)
    $cancelButton.Anchor = (
        [System.Windows.Forms.AnchorStyles]::Bottom -bor
        [System.Windows.Forms.AnchorStyles]::Right
    )

    $form.Controls.AddRange(@(
        $rootLabel,
        $rootBox,
        $browseButton,
        $refreshButton,
        $noteLabel,
        $foldersLabel,
        $folderList,
        $selectAllButton,
        $clearAllButton,
        $modeGroup,
        $outputLabel,
        $statusLabel,
        $startButton,
        $cancelButton
    ))

    $state = [pscustomobject]@{
        Accepted = $false
        Root = ""
        Folders = @()
        ForceRescan = $false
    }

    $context = @{
        LastDetectedRoot = ""
        PreferredRoot = $InitialRoot
        PreferredFolders = @($InitialFolders)
    }

    $reloadFolders = {
        param([bool]$ShowErrors)

        $candidateRoot = Normalize-RootPath $rootBox.Text
        if (-not [string]::IsNullOrWhiteSpace($candidateRoot)) {
            $rootBox.Text = $candidateRoot
        }
        try {
            $detectedFolders = @(Get-FirstLevelFolders $candidateRoot)
            if ($detectedFolders.Count -eq 0) {
                throw $Ui.NoFolders
            }

            $checkedBefore = @{}
            for ($i = 0; $i -lt $folderList.Items.Count; $i++) {
                if ($folderList.GetItemChecked($i)) {
                    $checkedBefore[$folderList.Items[$i].ToString()] = $true
                }
            }

            $preferred = @{}
            if ($candidateRoot.Equals($context.PreferredRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                foreach ($name in $context.PreferredFolders) {
                    if (-not [string]::IsNullOrWhiteSpace($name)) {
                        $preferred[$name] = $true
                    }
                }
            }

            $sameRoot = $candidateRoot.Equals(
                $context.LastDetectedRoot,
                [System.StringComparison]::OrdinalIgnoreCase
            )

            $folderList.BeginUpdate()
            try {
                $folderList.Items.Clear()
                foreach ($name in $detectedFolders) {
                    $isChecked = $true
                    if ($sameRoot -and $checkedBefore.Count -gt 0) {
                        $isChecked = $checkedBefore.ContainsKey($name)
                    }
                    elseif ($preferred.Count -gt 0) {
                        $isChecked = $preferred.ContainsKey($name)
                    }
                    [void]$folderList.Items.Add($name, $isChecked)
                }
            }
            finally {
                $folderList.EndUpdate()
            }

            $context.LastDetectedRoot = $candidateRoot
            $statusLabel.Text = [string]::Format($Ui.StatusDetected, $detectedFolders.Count)
            $statusLabel.ForeColor = [System.Drawing.Color]::DarkGreen
            return $true
        }
        catch {
            $folderList.Items.Clear()
            $statusLabel.Text = $_.Exception.Message
            $statusLabel.ForeColor = [System.Drawing.Color]::DarkRed

            if ($ShowErrors) {
                [void][System.Windows.Forms.MessageBox]::Show(
                    $form,
                    ($Ui.DetectFailed + [Environment]::NewLine + $_.Exception.Message),
                    $Ui.ErrorTitle,
                    [System.Windows.Forms.MessageBoxButtons]::OK,
                    [System.Windows.Forms.MessageBoxIcon]::Error
                )
            }
            return $false
        }
    }

    $browseButton.Add_Click({
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = $Ui.RootLabel
        $dialog.ShowNewFolderButton = $false

        $browseRoot = Normalize-RootPath $rootBox.Text
        if (Test-Path -LiteralPath $browseRoot -PathType Container) {
            $dialog.SelectedPath = $browseRoot
        }

        try {
            if ($dialog.ShowDialog($form) -eq [System.Windows.Forms.DialogResult]::OK) {
                $rootBox.Text = $dialog.SelectedPath
                [void](& $reloadFolders $true)
            }
        }
        finally {
            $dialog.Dispose()
        }
    })

    $refreshButton.Add_Click({
        [void](& $reloadFolders $true)
    })

    $selectAllButton.Add_Click({
        for ($i = 0; $i -lt $folderList.Items.Count; $i++) {
            $folderList.SetItemChecked($i, $true)
        }
    })

    $clearAllButton.Add_Click({
        for ($i = 0; $i -lt $folderList.Items.Count; $i++) {
            $folderList.SetItemChecked($i, $false)
        }
    })

    $startButton.Add_Click({
        $candidateRoot = Normalize-RootPath $rootBox.Text
        if (-not [string]::IsNullOrWhiteSpace($candidateRoot)) {
            $rootBox.Text = $candidateRoot
        }
        if ([string]::IsNullOrWhiteSpace($candidateRoot)) {
            [void][System.Windows.Forms.MessageBox]::Show(
                $form,
                $Ui.RootMissing,
                $Ui.ErrorTitle,
                [System.Windows.Forms.MessageBoxButtons]::OK,
                [System.Windows.Forms.MessageBoxIcon]::Warning
            )
            return
        }

        if (-not $candidateRoot.Equals(
            $context.LastDetectedRoot,
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            if (-not (& $reloadFolders $true)) {
                return
            }
        }

        $selected = New-Object 'System.Collections.Generic.List[string]'
        for ($i = 0; $i -lt $folderList.Items.Count; $i++) {
            if ($folderList.GetItemChecked($i)) {
                $selected.Add($folderList.Items[$i].ToString())
            }
        }

        if ($selected.Count -eq 0) {
            [void][System.Windows.Forms.MessageBox]::Show(
                $form,
                $Ui.SelectFolder,
                $Ui.ErrorTitle,
                [System.Windows.Forms.MessageBoxButtons]::OK,
                [System.Windows.Forms.MessageBoxIcon]::Warning
            )
            return
        }

        $state.Accepted = $true
        $state.Root = $candidateRoot
        $state.Folders = [string[]]$selected.ToArray()
        $state.ForceRescan = [bool]$forceRadio.Checked

        Save-UiSettings `
            -Path $settingsPath `
            -SelectedRoot $state.Root `
            -SelectedFolders $state.Folders `
            -SelectedForceRescan $state.ForceRescan

        $form.DialogResult = [System.Windows.Forms.DialogResult]::OK
        $form.Close()
    })

    $cancelButton.Add_Click({
        $form.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
        $form.Close()
    })

    $form.AcceptButton = $startButton
    $form.CancelButton = $cancelButton

    $form.Add_Shown({
        if (-not [string]::IsNullOrWhiteSpace($rootBox.Text)) {
            [void](& $reloadFolders $false)
        }
        else {
            $rootBox.Focus()
        }
    })

    try {
        [void]$form.ShowDialog()
    }
    finally {
        $form.Dispose()
    }

    return $state
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
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }

        $command = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $command) {
            return $command.Source
        }
    }

    throw "ffprobe.exe was not found. Put it beside this script or use -Ffprobe with a full path."
}

function Normalize-TextField {
    param($Value)

    if ($null -eq $Value) {
        return ""
    }

    $text = $Value.ToString().Trim()
    $text = [System.Text.RegularExpressions.Regex]::Replace($text, "[\r\n\t]+", " ")
    $text = $text -replace "\|", $safePipeReplacement
    return $text.Trim()
}

function Test-BadText {
    param($Value)

    if ($null -eq $Value) {
        return $true
    }

    $text = $Value.ToString().Trim()
    if ([string]::IsNullOrWhiteSpace($text)) {
        return $true
    }

    if ($text.Contains([string][char]0xFFFD)) {
        return $true
    }

    $badPatterns = @(
        (Decode-Utf8 "w4M="),
        (Decode-Utf8 "w4I="),
        (Decode-Utf8 "w6Pigqw="),
        (Decode-Utf8 "w6PigJo="),
        (Decode-Utf8 "w6PGkg=="),
        (Decode-Utf8 "w6Y="),
        (Decode-Utf8 "w6c="),
        (Decode-Utf8 "w6g="),
        (Decode-Utf8 "w6k="),
        (Decode-Utf8 "w6U="),
        (Decode-Utf8 "w6Q=")
    )

    foreach ($pattern in $badPatterns) {
        if ($text.Contains($pattern)) {
            return $true
        }
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

    $artist = $unknownArtist
    $title = $NameWithoutExtension
    $parts = $NameWithoutExtension -split "\s+[-\u2013\u2014]\s+", 2

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

    if ($null -eq $Tags) {
        return ""
    }

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
        ProbeSucceeded = $false
        DurationMs = 0
        Title = ""
        Artist = ""
        Album = ""
    }

    try {
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
                Write-Log ("ffprobe failed: {0}; {1}" -f $FilePath, $stderrText.Trim()) "WARN"
            }
            return $result
        }

        if ([string]::IsNullOrWhiteSpace($jsonText)) {
            return $result
        }

        $info = $jsonText | ConvertFrom-Json
        if ($null -eq $info -or $null -eq $info.format) {
            return $result
        }

        $result.ProbeSucceeded = $true

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
        Write-Log ("Media metadata exception: {0}; {1}" -f $FilePath, $_.Exception.Message) "WARN"
        return $result
    }
}

function Get-CacheFilePath {
    param(
        [string]$SourceRoot,
        [string]$CacheDirectory
    )

    Ensure-Directory $CacheDirectory

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
        Write-Log ("Cache load failed; metadata will be reprobed: {0}; {1}" -f $CachePath, $_.Exception.Message) "WARN"
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
        Version = 2
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
        }
    }

    $tempListPath = $ListPath + ".tmp"
    [System.IO.File]::WriteAllText($tempListPath, $newText, $Encoding)

    if (Test-Path -LiteralPath $ListPath -PathType Leaf) {
        Copy-Item -LiteralPath $tempListPath -Destination $ListPath -Force
        Remove-Item -LiteralPath $tempListPath -Force -ErrorAction SilentlyContinue
    }
    else {
        Move-Item -LiteralPath $tempListPath -Destination $ListPath -Force
    }

    return $true
}

$forceRescanRequested = [bool]($ForceProbe -or $ForceRescan)

if ($uiWasUsed) {
    Ensure-Directory $CacheRoot
    $savedSettings = Load-UiSettings $settingsPath

    $initialRoot = Normalize-RootPath $Root
    $initialFolders = @($Folders)
    $initialForceRescan = $forceRescanRequested

    if ($null -ne $savedSettings) {
        if ([string]::IsNullOrWhiteSpace($initialRoot) -and
            -not [string]::IsNullOrWhiteSpace($savedSettings.Root)) {
            $initialRoot = Normalize-RootPath $savedSettings.Root.ToString()
        }

        if ($initialFolders.Count -eq 0 -and $null -ne $savedSettings.Folders) {
            $initialFolders = @($savedSettings.Folders | ForEach-Object { $_.ToString() })
        }

        if (-not $PSBoundParameters.ContainsKey("ForceProbe") -and
            -not $PSBoundParameters.ContainsKey("ForceRescan") -and
            $null -ne $savedSettings.ForceRescan) {
            $initialForceRescan = [bool]$savedSettings.ForceRescan
        }
    }

    $setup = Show-SetupDialog `
        -InitialRoot $initialRoot `
        -InitialFolders $initialFolders `
        -InitialForceRescan $initialForceRescan `
        -OutputPath $SourcesOut

    if (-not $setup.Accepted) {
        exit 0
    }

    $Root = Normalize-RootPath $setup.Root
    $Folders = @($setup.Folders)
    $forceRescanRequested = [bool]$setup.ForceRescan
}
else {
    $Root = Normalize-RootPath $Root

    if ([string]::IsNullOrWhiteSpace($Root)) {
        throw "Use -Root to specify the NAS music root when -NoUi is used."
    }

    if ($Folders.Count -eq 0) {
        $Folders = @(Get-FirstLevelFolders $Root)
    }
}

try {
    Set-Content -LiteralPath $logPath -Value ("NAS multi-library generator - " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss")) -Encoding UTF8

    Write-Host "=============================================" -ForegroundColor Cyan
    Write-Host " ESP32 NAS multi-library generator v5.2" -ForegroundColor Cyan
    Write-Host "=============================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Log ("Root: {0}" -f $Root)
    Write-Log ("Log: {0}" -f $logPath)
    Write-Log ("Cache: {0}" -f $CacheRoot)
    Write-Log ("Mode: {0}" -f $(if ($forceRescanRequested) { "force rescan" } else { "incremental" }))

    if ($forceRescanRequested) {
        Write-Log "All metadata cache entries will be ignored in this run." "WARN"
    }

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw ("NAS root is missing or inaccessible: {0}" -f $Root)
    }

    $resolvedFfprobe = Resolve-FfprobePath $Ffprobe
    Write-Log ("ffprobe: {0}" -f $resolvedFfprobe)

    $extensionSet = @{}
    foreach ($extension in $Extensions) {
        if ([string]::IsNullOrWhiteSpace($extension)) {
            continue
        }

        $normalizedExtension = $extension.Trim().ToLowerInvariant()
        if (-not $normalizedExtension.StartsWith('.')) {
            $normalizedExtension = '.' + $normalizedExtension
        }
        $extensionSet[$normalizedExtension] = $true
    }

    if ($extensionSet.Count -eq 0) {
        throw "No audio file extensions are configured."
    }

    $sourceLines = New-Object 'System.Collections.Generic.List[string]'
    $totalSongs = 0
    $totalReused = 0
    $totalProbed = 0
    $totalListsWritten = 0

    foreach ($folder in $Folders) {
        if ([string]::IsNullOrWhiteSpace($folder)) {
            continue
        }

        $displayName = $folder.Trim()
        $sourceRelative = ($displayName -replace "\\", "/").Trim([char]'/')
        $sourceRoot = Join-Path $Root $displayName

        if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
            Write-Log ("Skip missing library folder: {0}" -f $sourceRoot) "WARN"
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
            Write-Log ("Building library: {0} ({1} tracks)" -f $displayName, $total)

            foreach ($file in $files) {
                $index++
                if (($index % 50) -eq 0 -or $index -eq $total) {
                    Write-Host ("  Progress: {0} / {1}; cache={2}; ffprobe={3}" -f $index, $total, $reusedCount, $probedCount)
                }

                if (-not $file.FullName.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                    Write-Log ("Skip file outside source root: {0}" -f $file.FullName) "WARN"
                    continue
                }

                $relativePath = $file.FullName.Substring($rootPrefix.Length).TrimStart($pathTrimChars)
                $relativePath = $relativePath -replace "\\", "/"
                $cacheKey = $relativePath.ToLowerInvariant()
                $cachedEntry = $oldCache[$cacheKey]
                $mediaInfo = $null

                $cacheValid = $false
                if (-not $forceRescanRequested -and $null -ne $cachedEntry) {
                    try {
                        $cachedProbeSucceeded = $false
                        if ($null -ne $cachedEntry.PSObject.Properties["ProbeSucceeded"]) {
                            $cachedProbeSucceeded = [bool]$cachedEntry.ProbeSucceeded
                        }
                        else {
                            $cachedProbeSucceeded = ([int]$cachedEntry.DurationMs -gt 0)
                        }

                        $cacheValid = (
                            $cachedProbeSucceeded -and
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
                        ProbeSucceeded = $true
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
                $album = Normalize-TextField (First-GoodText $mediaInfo.Album $unknownAlbum)

                if ([string]::IsNullOrWhiteSpace($title)) {
                    $title = $nameWithoutExtension
                }
                if ([string]::IsNullOrWhiteSpace($artist)) {
                    $artist = $unknownArtist
                }
                if ([string]::IsNullOrWhiteSpace($album)) {
                    $album = $unknownAlbum
                }

                $format = $file.Extension.TrimStart('.').ToLowerInvariant()
                $durationMs = [int]$mediaInfo.DurationMs
                $line = "{0}|{1}|{2}|{3}|{4}|{5}" -f `
                    $title, `
                    $relativePath, `
                    $format, `
                    $artist, `
                    $album, `
                    $durationMs

                $items.Add($line)
                $nextCacheEntries.Add([pscustomobject][ordered]@{
                    RelativePath = $relativePath
                    Length = [int64]$file.Length
                    LastWriteTimeUtcTicks = [int64]$file.LastWriteTimeUtc.Ticks
                    ProbeSucceeded = [bool]$mediaInfo.ProbeSucceeded
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
            if ($listWritten) {
                $totalListsWritten++
            }

            Write-Log (
                "Completed: {0}; tracks={1}; cache={2}; ffprobe={3}; list_written={4}" -f `
                    $displayName, `
                    $items.Count, `
                    $reusedCount, `
                    $probedCount, `
                    $listWritten
            )
        }
        catch {
            Write-Log ("Library failed: {0}; {1}" -f $displayName, $_.Exception.Message) "ERROR"
            continue
        }
    }

    if ($sourceLines.Count -eq 0) {
        throw "No NAS library list was generated. Check folder selection, NAS permissions, and the log."
    }

    $sourcesDirectory = Split-Path -Parent $SourcesOut
    if (-not [string]::IsNullOrWhiteSpace($sourcesDirectory)) {
        Ensure-Directory $sourcesDirectory
    }

    [System.IO.File]::WriteAllLines(
        $SourcesOut,
        [string[]]$sourceLines.ToArray(),
        $utf8WithoutBom
    )

    Write-Host ""
    Write-Host "Completed" -ForegroundColor Green
    Write-Log ("Libraries: {0}" -f $sourceLines.Count)
    Write-Log ("Tracks: {0}" -f $totalSongs)
    Write-Log ("Cache reused: {0}" -f $totalReused)
    Write-Log ("ffprobe runs: {0}" -f $totalProbed)
    Write-Log ("List files updated: {0}" -f $totalListsWritten)
    Write-Log ("Sources file: {0}" -f $SourcesOut)

    if ($uiWasUsed) {
        Add-Type -AssemblyName System.Windows.Forms
        $summary = (
            "{0}`r`n`r`nLibraries: {1}`r`nTracks: {2}`r`nCache reused: {3}`r`nffprobe: {4}`r`n`r`n{5}{6}" -f `
                $Ui.Done, `
                $sourceLines.Count, `
                $totalSongs, `
                $totalReused, `
                $totalProbed, `
                $Ui.OpenLog, `
                $logPath
        )

        [void][System.Windows.Forms.MessageBox]::Show(
            $summary,
            $Ui.DoneTitle,
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Information
        )
    }
}
catch {
    $exitCode = 1
    $errorMessage = $_.Exception.Message

    Write-Host ""
    try {
        Write-Log ("Execution failed: {0}" -f $errorMessage) "ERROR"
    }
    catch {
        Write-Host ("Execution failed: {0}" -f $errorMessage) -ForegroundColor Red
    }

    if ($uiWasUsed) {
        Add-Type -AssemblyName System.Windows.Forms
        [void][System.Windows.Forms.MessageBox]::Show(
            ($Ui.Failed + $errorMessage + [Environment]::NewLine + [Environment]::NewLine + $Ui.OpenLog + $logPath),
            $Ui.ErrorTitle,
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Error
        )
    }
}
finally {
    if (-not $NoPause -and -not $uiWasUsed) {
        Write-Host ""
        [void](Read-Host "Press Enter to close")
    }
}

exit $exitCode
