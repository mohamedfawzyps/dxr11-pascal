# A small installer and switchboard for the shim.
#
# Everything here is something you can do by hand: copy three files into a
# folder, write a two line .ini, read a log. It exists because "copy a DLL next
# to the .exe" is a sentence that loses people, and because the log is the
# product's main diagnostic and nobody opens it.
#
# WinForms through PowerShell rather than a compiled app, deliberately. It adds
# no toolchain, it is readable by anyone who wants to check what it does to
# their game folder before running it, and the whole job is copying files.
#
#   powershell -ExecutionPolicy Bypass -File tools\dxr-tier-11-setup.ps1
#
# or run tools\dxr-tier-11-setup.bat, which does that for you.

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ErrorActionPreference = 'Stop'

# The shim and its two DXC dependencies, looked for beside this script first
# (a release layout) and then one directory up (a build tree).
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$roots = @($here, (Split-Path -Parent $here))
function Find-Source([string]$name) {
    foreach ($r in $roots) {
        $p = Join-Path $r $name
        if (Test-Path $p) { return $p }
    }
    return $null
}

$script:targetDir = ''

# --- window -----------------------------------------------------------------
$form = New-Object Windows.Forms.Form
$form.Text = 'DXR Tier 1.1 for Pascal'
$form.Size = New-Object Drawing.Size(660, 760)
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object Drawing.Font('Segoe UI', 9)

function Add-Label($text, $x, $y, $w, $bold = $false) {
    $l = New-Object Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object Drawing.Point($x, $y)
    $l.Size = New-Object Drawing.Size($w, 20)
    if ($bold) { $l.Font = New-Object Drawing.Font('Segoe UI', 9, [Drawing.FontStyle]::Bold) }
    $form.Controls.Add($l); return $l
}

Add-Label '1. Pick the game or editor .exe' 15 15 300 $true | Out-Null

$txtExe = New-Object Windows.Forms.TextBox
$txtExe.Location = New-Object Drawing.Point(15, 40)
$txtExe.Size = New-Object Drawing.Size(520, 24)
$txtExe.ReadOnly = $true
$form.Controls.Add($txtExe)

$btnBrowse = New-Object Windows.Forms.Button
$btnBrowse.Text = 'Browse...'
$btnBrowse.Location = New-Object Drawing.Point(545, 39)
$btnBrowse.Size = New-Object Drawing.Size(90, 26)
$form.Controls.Add($btnBrowse)

$lblHint = Add-Label 'Unreal Editor is Engine\Binaries\Win64\UnrealEditor.exe' 15 68 620
$lblHint.ForeColor = [Drawing.Color]::Gray

Add-Label '2. Status' 15 100 300 $true | Out-Null
$lblStatus = Add-Label '' 15 124 620
$lblDeps = Add-Label '' 15 146 620
$lblDeps.ForeColor = [Drawing.Color]::Gray

$btnInstall = New-Object Windows.Forms.Button
$btnInstall.Text = 'Install'
$btnInstall.Location = New-Object Drawing.Point(15, 174)
$btnInstall.Size = New-Object Drawing.Size(120, 30)
$form.Controls.Add($btnInstall)

$btnRemove = New-Object Windows.Forms.Button
$btnRemove.Text = 'Remove'
$btnRemove.Location = New-Object Drawing.Point(145, 174)
$btnRemove.Size = New-Object Drawing.Size(120, 30)
$form.Controls.Add($btnRemove)

# Only appears when DXC is missing, because that is the only time it means
# anything. A button that is always there is a button nobody reads.
$btnGetDxc = New-Object Windows.Forms.Button
$btnGetDxc.Text = 'Get DXC...'
$btnGetDxc.Location = New-Object Drawing.Point(285, 174)
$btnGetDxc.Size = New-Object Drawing.Size(120, 30)
$btnGetDxc.Visible = $false
$form.Controls.Add($btnGetDxc)

# The versions of every DLL that decides whether a run works, none of which
# are ours. Read off the FILES here; the log reports what the process actually
# loaded, which can differ if the game carries its own copy somewhere else.
$grpVers = New-Object Windows.Forms.GroupBox
$grpVers.Text = 'Versions in the game folder'
$grpVers.Location = New-Object Drawing.Point(15, 214)
$grpVers.Size = New-Object Drawing.Size(620, 118)
$form.Controls.Add($grpVers)

$txtVers = New-Object Windows.Forms.TextBox
$txtVers.Multiline = $true
$txtVers.ReadOnly = $true
$txtVers.Location = New-Object Drawing.Point(12, 22)
$txtVers.Size = New-Object Drawing.Size(596, 86)
$txtVers.BorderStyle = 'None'
$txtVers.BackColor = $grpVers.BackColor
$txtVers.Font = New-Object Drawing.Font('Consolas', 8.5)
$grpVers.Controls.Add($txtVers)

Add-Label '3. Settings' 15 348 300 $true | Out-Null

$chkTier = New-Object Windows.Forms.CheckBox
$chkTier.Text = 'Report DXR Tier 1.1 and rewrite RayQuery shaders'
$chkTier.Location = New-Object Drawing.Point(15, 374)
$chkTier.Size = New-Object Drawing.Size(500, 22)
$chkTier.Checked = $true
$form.Controls.Add($chkTier)

$grpDebug = New-Object Windows.Forms.GroupBox
$grpDebug.Text = 'Debug options'
$grpDebug.Location = New-Object Drawing.Point(15, 404)
$grpDebug.Size = New-Object Drawing.Size(620, 124)
$form.Controls.Add($grpDebug)

$chkNoWrap = New-Object Windows.Forms.CheckBox
$chkNoWrap.Text = 'Translate nothing (nowrap)'
$chkNoWrap.Location = New-Object Drawing.Point(12, 24)
$chkNoWrap.Size = New-Object Drawing.Size(560, 22)
$grpDebug.Controls.Add($chkNoWrap)

$lblNoWrapWhy = New-Object Windows.Forms.Label
$lblNoWrapWhy.Text = 'Leaves the DLL loaded but switches the whole layer off, including Tier 1.1. If a problem survives this, it is not the shim.'
$lblNoWrapWhy.Location = New-Object Drawing.Point(30, 46)
$lblNoWrapWhy.Size = New-Object Drawing.Size(580, 22)
$lblNoWrapWhy.ForeColor = [Drawing.Color]::Gray
$grpDebug.Controls.Add($lblNoWrapWhy)

# Not a setting anybody wants on permanently, but the one thing that turns
# "the shim refused 158 shaders" into 158 files somebody can actually read.
$chkDump = New-Object Windows.Forms.CheckBox
$chkDump.Text = 'Save shaders the shim could not translate'
$chkDump.Location = New-Object Drawing.Point(12, 72)
$chkDump.Size = New-Object Drawing.Size(560, 22)
$grpDebug.Controls.Add($chkDump)

$lblDumpWhy = New-Object Windows.Forms.Label
$lblDumpWhy.Text = 'Writes them to a refused-shaders folder beside the game, up to 64. Only useful for reporting a problem.'
$lblDumpWhy.Location = New-Object Drawing.Point(30, 94)
$lblDumpWhy.Size = New-Object Drawing.Size(580, 22)
$lblDumpWhy.ForeColor = [Drawing.Color]::Gray
$grpDebug.Controls.Add($lblDumpWhy)

Add-Label '4. Then just launch the game normally' 15 542 400 $true | Out-Null

$lblLogWhere = Add-Label 'Log file, blank for the default in %TEMP%. A folder is fine; the usual name goes in it.' 15 570 620
$lblLogWhere.ForeColor = [Drawing.Color]::Gray

$txtLog = New-Object Windows.Forms.TextBox
$txtLog.Location = New-Object Drawing.Point(15, 592)
$txtLog.Size = New-Object Drawing.Size(425, 24)
$form.Controls.Add($txtLog)

$btnLogBrowse = New-Object Windows.Forms.Button
$btnLogBrowse.Text = 'Browse...'
$btnLogBrowse.Location = New-Object Drawing.Point(448, 591)
$btnLogBrowse.Size = New-Object Drawing.Size(90, 26)
$form.Controls.Add($btnLogBrowse)

$btnLogDefault = New-Object Windows.Forms.Button
$btnLogDefault.Text = 'Default'
$btnLogDefault.Location = New-Object Drawing.Point(545, 591)
$btnLogDefault.Size = New-Object Drawing.Size(90, 26)
$form.Controls.Add($btnLogDefault)

$btnLog = New-Object Windows.Forms.Button
$btnLog.Text = 'Open the log'
$btnLog.Location = New-Object Drawing.Point(15, 630)
$btnLog.Size = New-Object Drawing.Size(130, 30)
$form.Controls.Add($btnLog)

$btnProblems = New-Object Windows.Forms.Button
$btnProblems.Text = 'Show refusals only'
$btnProblems.Location = New-Object Drawing.Point(155, 630)
$btnProblems.Size = New-Object Drawing.Size(150, 30)
$form.Controls.Add($btnProblems)

$lblLogHint = Add-Label '' 15 668 620
$lblLogHint.ForeColor = [Drawing.Color]::Gray

# --- behaviour ---------------------------------------------------------------

# Where the log actually is, resolved the same way the shim resolves it, so
# "Open the log" opens the file the shim wrote rather than the default one.
#
# Kept deliberately in step with OpenLogFile in proxy/d3d12_proxy.cpp: blank
# means %TEMP%, a folder gets the usual name put in it, and a relative path is
# relative to the game folder and not to wherever this script was started.
function Get-LogPath {
    $where = ''
    if ($txtLog -and $txtLog.Text) { $where = $txtLog.Text.Trim() }
    if (-not $where) { return (Join-Path $env:TEMP 'dxr-tier-11-proxy.log') }

    if (-not [IO.Path]::IsPathRooted($where)) {
        if ($script:targetDir) { $where = Join-Path $script:targetDir $where }
        else { return (Join-Path $env:TEMP 'dxr-tier-11-proxy.log') }
    }
    if (Test-Path $where -PathType Container) {
        return (Join-Path $where 'dxr-tier-11-proxy.log')
    }
    return $where
}

function Update-Status {
    if (-not $script:targetDir) {
        $lblStatus.Text = 'No .exe chosen yet.'
        $lblStatus.ForeColor = [Drawing.Color]::Gray
        $lblDeps.Text = ''
        $btnInstall.Enabled = $false; $btnRemove.Enabled = $false
        $chkTier.Enabled = $false; $chkNoWrap.Enabled = $false
        $chkDump.Enabled = $false
        $txtLog.Enabled = $false; $btnLogBrowse.Enabled = $false
        $btnLogDefault.Enabled = $false
        return
    }
    $dll = Join-Path $script:targetDir 'd3d12.dll'
    $installed = Test-Path $dll
    $btnInstall.Enabled = -not $installed
    $btnRemove.Enabled = $installed
    $chkTier.Enabled = $installed; $chkNoWrap.Enabled = $installed
    $chkDump.Enabled = $installed
    $txtLog.Enabled = $installed; $btnLogBrowse.Enabled = $installed
    $btnLogDefault.Enabled = $installed

    if ($installed) {
        $when = (Get-Item $dll).LastWriteTime.ToString('yyyy-MM-dd HH:mm')
        $lblStatus.Text = "Installed. d3d12.dll copied $when."
        $lblStatus.ForeColor = [Drawing.Color]::FromArgb(15, 110, 86)
        $haveDxc = (Test-Path (Join-Path $script:targetDir 'dxcompiler.dll')) -and
                   (Test-Path (Join-Path $script:targetDir 'dxil.dll'))
        if ($haveDxc) {
            $lblDeps.Text = 'dxcompiler.dll and dxil.dll are present, so RayQuery shaders can be rewritten.'
            $lblDeps.ForeColor = [Drawing.Color]::Gray
            $btnGetDxc.Visible = $false
        } else {
            $lblDeps.Text = 'dxcompiler.dll and dxil.dll are MISSING, so ray tracing will NOT switch on. The game still runs normally without them.'
            $lblDeps.ForeColor = [Drawing.Color]::FromArgb(163, 45, 45)
            $btnGetDxc.Visible = $true
        }
    } else {
        $lblStatus.Text = 'Not installed.'
        $lblStatus.ForeColor = [Drawing.Color]::FromArgb(163, 45, 45)
        $lblDeps.Text = ''
        $btnGetDxc.Visible = $false
    }
    Update-Versions
    Read-Ini
}

# What is actually sitting in the game folder, and which version.
#
# Five DLLs decide whether a run works and only one of them is ours. Reading
# them here answers the first question about any report without asking the
# person to go and look, and it answers it BEFORE the game is launched, which
# the log cannot do.
function Update-Versions {
    if (-not $script:targetDir) { $txtVers.Text = ''; return }

    # Beside the exe, for the three that must be there.
    function Find-Beside($name) {
        $p = Join-Path $script:targetDir $name
        if (Test-Path $p) { return $p }
        return $null
    }

    # The Agility SDK is NOT beside the exe. `D3D12SDKPath` is a relative path
    # the application chooses, and real games nest it: Escher uses
    # `Binaries\Win64\D3D12\x64`. Guessing a list of subfolder names missed
    # that and reported "the game uses the Windows D3D12", which was wrong and
    # was the kind of wrong that looks like an answer. So search instead.
    #
    # Bounded to three levels, because this runs on a game folder and the point
    # is to find a redirect target, not to walk the whole install.
    function Find-Agility($name) {
        $hit = Get-ChildItem -Path $script:targetDir -Filter $name -Recurse -Depth 3 `
                             -File -Force -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
        return $null
    }

    $rows = @(
        @('d3d12.dll (this shim)', (Find-Beside 'd3d12.dll'),      'not installed'),
        @('dxgi.dll (device id)',  (Find-Beside 'dxgi.dll'),       'not installed, Unreal will refuse Pascal'),
        @('dxcompiler.dll',        (Find-Beside 'dxcompiler.dll'), 'MISSING, ray tracing will not switch on'),
        @('dxil.dll',              (Find-Beside 'dxil.dll'),       'MISSING, ray tracing will not switch on'),
        @('D3D12Core.dll',         (Find-Agility 'D3D12Core.dll'),      'none found, the game uses the Windows D3D12'),
        @('d3d12SDKLayers.dll',    (Find-Agility 'd3d12SDKLayers.dll'), 'none found, normal')
    )

    $lines = foreach ($r in $rows) {
        if ($r[1]) {
            $v = (Get-Item $r[1]).VersionInfo.FileVersion
            if (-not $v) { $v = '(no version resource)' }
            # Where it was found matters as much as the version for the two
            # that are not beside the exe.
            $sub = Split-Path $r[1] -Parent
            if ($sub -eq $script:targetDir) { $where = '' }
            else { $where = '   in .\' + $sub.Substring($script:targetDir.Length).TrimStart('\') }
            '{0,-22} {1}{2}' -f $r[0], $v.Trim(), $where
        } else {
            '{0,-22} {1}' -f $r[0], $r[2]
        }
    }
    $txtVers.Lines = @($lines)
}

function Read-Ini {
    $ini = Join-Path $script:targetDir 'dxr-tier-11.ini'
    $tier = $true; $nowrap = $false; $log = ''; $dump = ''
    if (Test-Path $ini) {
        foreach ($line in Get-Content $ini) {
            $t = $line.Trim()
            if ($t -eq '' -or $t.StartsWith('#') -or $t.StartsWith(';')) { continue }
            $kv = $t -split '=', 2
            if ($kv.Count -ne 2) { continue }
            $k = $kv[0].Trim().ToLower()
            # The VALUE is not lowered any more. A path is not a keyword, and
            # the shim stopped lowering it too.
            $raw = $kv[1].Trim()
            $on = @('1', 'true', 'on', 'yes') -contains $raw.ToLower()
            if ($k -eq 'tier11') { $tier = $on }
            if ($k -eq 'nowrap') { $nowrap = $on }
            if ($k -eq 'log')    { $log = $raw }
            if ($k -eq 'dump')   { $dump = $raw }
        }
    }
    $script:suppress = $true
    $chkTier.Checked = $tier; $chkNoWrap.Checked = $nowrap
    $chkDump.Checked = [bool]$dump
    $txtLog.Text = $log
    $script:suppress = $false
}

function Write-Ini {
    if ($script:suppress -or -not $script:targetDir) { return }
    $ini = Join-Path $script:targetDir 'dxr-tier-11.ini'
    $t = if ($chkTier.Checked) { '1' } else { '0' }
    $n = if ($chkNoWrap.Checked) { '1' } else { '0' }
    $lines = @(
        '; Written by dxr-tier-11-setup. Safe to edit by hand.',
        "tier11 = $t",
        "nowrap = $n"
    )
    # Omitted entirely when blank, so the file says nothing about a setting the
    # user did not set and the shim keeps its own default.
    $where = $txtLog.Text.Trim()
    if ($where) { $lines += "log = $where" }
    # A relative path, so the folder lands beside the game whatever the ini is
    # later copied to.
    if ($chkDump.Checked) { $lines += 'dump = refused-shaders' }

    # UTF-8 with no BOM, written through .NET because Set-Content -Encoding
    # ascii turns any non-ASCII character in a path into a question mark, and
    # -Encoding utf8 in Windows PowerShell adds a BOM. The shim reads UTF-8 and
    # falls back to the system code page for a file edited by hand.
    [IO.File]::WriteAllLines($ini, $lines, (New-Object Text.UTF8Encoding $false))
    $lblLogHint.Text = "Saved to $ini"
}

function Open-DxcPage {
    Start-Process 'https://github.com/microsoft/DirectXShaderCompiler/releases'
    [Windows.Forms.MessageBox]::Show(
        "On that page, open the newest release and download the .zip.`n`n" +
        "Inside it, go to bin\x64\ and copy these two files:`n" +
        "    dxcompiler.dll`n" +
        "    dxil.dll`n`n" +
        "Put them in:`n$script:targetDir`n`n" +
        "Take both from the SAME release. A mismatched pair fails to sign.`n`n" +
        "Then come back here and the status will turn green.",
        'What to download', 'OK', 'Information') | Out-Null
}

$btnBrowse.Add_Click({
    $d = New-Object Windows.Forms.OpenFileDialog
    $d.Filter = 'Programs (*.exe)|*.exe'
    $d.Title = 'Pick the game or editor executable'
    if ($d.ShowDialog() -eq 'OK') {
        $txtExe.Text = $d.FileName
        $script:targetDir = Split-Path -Parent $d.FileName
        Update-Status
    }
})

$btnInstall.Add_Click({
    $src = Find-Source 'd3d12.dll'
    if (-not $src) {
        [Windows.Forms.MessageBox]::Show(
            "d3d12.dll was not found beside this script or one folder up. Build it with build_proxy.bat, or run this from the folder you extracted the release into.",
            'Nothing to install', 'OK', 'Error') | Out-Null
        return
    }
    Copy-Item $src (Join-Path $script:targetDir 'd3d12.dll') -Force
    $copied = 'd3d12.dll'
    # dxgi.dll goes in with it. It exists only to report a Pascal card under a
    # Turing device id, because Unreal refuses ray tracing on Pascal by device
    # id AFTER accepting the tier. Without it the shim has nothing to do in an
    # Unreal game. It is a separate file so it can be deleted on its own.
    foreach ($n in @('dxgi.dll', 'dxcompiler.dll', 'dxil.dll')) {
        $p = Find-Source $n
        if ($p) { Copy-Item $p (Join-Path $script:targetDir $n) -Force; $copied += ", $n" }
    }
    $lblLogHint.Text = "Copied $copied"
    Update-Status

    $haveDxc = (Test-Path (Join-Path $script:targetDir 'dxcompiler.dll')) -and
               (Test-Path (Join-Path $script:targetDir 'dxil.dll'))
    if (-not $haveDxc) {
        # Said here rather than from inside the game. A dialog raised by a
        # proxy DLL during device creation blocks the application's startup
        # thread, and lands behind an exclusive fullscreen window where nobody
        # can see it. This is the moment BEFORE the mistake, in a program the
        # user opened on purpose.
        $answer = [Windows.Forms.MessageBox]::Show(
            "Installed, but ray tracing will not switch on yet.`n`n" +
            "The shim needs dxcompiler.dll and dxil.dll to rewrite ray tracing " +
            "shaders. They are not in the game folder, and they are not shipped " +
            "here because they belong to Microsoft.`n`n" +
            "Without them the game runs exactly as it did before, just without " +
            "ray tracing. Nothing will break.`n`n" +
            "Open the download page now?",
            'Two more files needed', 'YesNo', 'Warning')
        if ($answer -eq 'Yes') { Open-DxcPage }
    }
})

$btnGetDxc.Add_Click({ Open-DxcPage })

$btnRemove.Add_Click({
    $answer = [Windows.Forms.MessageBox]::Show(
        "Delete d3d12.dll, dxgi.dll and dxr-tier-11.ini from`n$script:targetDir ?`n`nThis restores the original behaviour exactly. dxcompiler.dll and dxil.dll are left alone, since the game may use them itself.",
        'Remove the shim', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { return }
    foreach ($n in @('d3d12.dll', 'dxgi.dll', 'dxr-tier-11.ini')) {
        $p = Join-Path $script:targetDir $n
        if (Test-Path $p) { Remove-Item $p -Force }
    }
    $lblLogHint.Text = 'Removed.'
    Update-Status
})

$chkTier.Add_CheckedChanged({ Write-Ini })
$chkNoWrap.Add_CheckedChanged({ Write-Ini })
$chkDump.Add_CheckedChanged({ Write-Ini })

$txtLog.Add_TextChanged({ Write-Ini })

$btnLogBrowse.Add_Click({
    # A SAVE dialog rather than an open one, because the file usually does not
    # exist yet: the point of setting this is to decide where it will go.
    $d = New-Object Windows.Forms.SaveFileDialog
    $d.Title = 'Where should the log go?'
    $d.Filter = 'Log files (*.log)|*.log|All files (*.*)|*.*'
    $d.FileName = 'dxr-tier-11-proxy.log'
    $d.OverwritePrompt = $false   # it is appended to, not replaced
    $cur = Get-LogPath
    $dir = Split-Path $cur -Parent
    if ($dir -and (Test-Path $dir)) { $d.InitialDirectory = $dir }
    if ($d.ShowDialog() -eq 'OK') { $txtLog.Text = $d.FileName }
})

$btnLogDefault.Add_Click({ $txtLog.Text = '' })

$btnLog.Add_Click({
    $logPath = Get-LogPath
    if (Test-Path $logPath) { Start-Process notepad.exe $logPath }
    else { $lblLogHint.Text = "No log yet at $logPath. Run the game once." }
})

$btnProblems.Add_Click({
    $logPath = Get-LogPath
    if (-not (Test-Path $logPath)) {
        $lblLogHint.Text = "No log yet at $logPath. Run the game once."
        return
    }
    $hits = Select-String -Path $logPath -Pattern 'REFUSED|NOT lowered|NOTE:|SKIPPED|FAILED' |
            ForEach-Object { $_.Line.Trim() } | Select-Object -Unique
    if (-not $hits) {
        [Windows.Forms.MessageBox]::Show(
            'Nothing was refused. Every shader the application sent was translated.',
            'Refusals', 'OK', 'Information') | Out-Null
        return
    }
    $tmp = Join-Path $env:TEMP 'dxr11_refusals.txt'
    $hits | Set-Content $tmp -Encoding utf8
    Start-Process notepad.exe $tmp
})

Update-Status
[void]$form.ShowDialog()
