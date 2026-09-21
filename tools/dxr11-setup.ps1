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
#   powershell -ExecutionPolicy Bypass -File tools\dxr11-setup.ps1
#
# or run tools\dxr11-setup.bat, which does that for you.

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
$form.Size = New-Object Drawing.Size(660, 520)
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

Add-Label '3. Settings' 15 220 300 $true | Out-Null

$chkTier = New-Object Windows.Forms.CheckBox
$chkTier.Text = 'Report DXR Tier 1.1 and rewrite RayQuery shaders'
$chkTier.Location = New-Object Drawing.Point(15, 246)
$chkTier.Size = New-Object Drawing.Size(500, 22)
$chkTier.Checked = $true
$form.Controls.Add($chkTier)

$grpDebug = New-Object Windows.Forms.GroupBox
$grpDebug.Text = 'Debug options'
$grpDebug.Location = New-Object Drawing.Point(15, 276)
$grpDebug.Size = New-Object Drawing.Size(620, 76)
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

Add-Label '4. Then just launch the game normally' 15 366 400 $true | Out-Null

$btnLog = New-Object Windows.Forms.Button
$btnLog.Text = 'Open the log'
$btnLog.Location = New-Object Drawing.Point(15, 394)
$btnLog.Size = New-Object Drawing.Size(130, 30)
$form.Controls.Add($btnLog)

$btnProblems = New-Object Windows.Forms.Button
$btnProblems.Text = 'Show refusals only'
$btnProblems.Location = New-Object Drawing.Point(155, 394)
$btnProblems.Size = New-Object Drawing.Size(150, 30)
$form.Controls.Add($btnProblems)

$lblLogHint = Add-Label '' 15 432 620
$lblLogHint.ForeColor = [Drawing.Color]::Gray

# --- behaviour ---------------------------------------------------------------
$logPath = Join-Path $env:TEMP 'dxr11_proxy.log'

function Update-Status {
    if (-not $script:targetDir) {
        $lblStatus.Text = 'No .exe chosen yet.'
        $lblStatus.ForeColor = [Drawing.Color]::Gray
        $lblDeps.Text = ''
        $btnInstall.Enabled = $false; $btnRemove.Enabled = $false
        $chkTier.Enabled = $false; $chkNoWrap.Enabled = $false
        return
    }
    $dll = Join-Path $script:targetDir 'd3d12.dll'
    $installed = Test-Path $dll
    $btnInstall.Enabled = -not $installed
    $btnRemove.Enabled = $installed
    $chkTier.Enabled = $installed; $chkNoWrap.Enabled = $installed

    if ($installed) {
        $when = (Get-Item $dll).LastWriteTime.ToString('yyyy-MM-dd HH:mm')
        $lblStatus.Text = "Installed. d3d12.dll copied $when. The version is in the log."
        $lblStatus.ForeColor = [Drawing.Color]::FromArgb(15, 110, 86)
        $haveDxc = (Test-Path (Join-Path $script:targetDir 'dxcompiler.dll')) -and
                   (Test-Path (Join-Path $script:targetDir 'dxil.dll'))
        if ($haveDxc) {
            $lblDeps.Text = 'dxcompiler.dll and dxil.dll are present, so RayQuery shaders can be rewritten.'
        } else {
            $lblDeps.Text = 'dxcompiler.dll and dxil.dll are MISSING. Ray tracing shaders cannot be rewritten without them. Get them from the DXC releases page and copy them here.'
        }
    } else {
        $lblStatus.Text = 'Not installed.'
        $lblStatus.ForeColor = [Drawing.Color]::FromArgb(163, 45, 45)
        $lblDeps.Text = ''
    }
    Read-Ini
}

function Read-Ini {
    $ini = Join-Path $script:targetDir 'dxr11.ini'
    $tier = $true; $nowrap = $false
    if (Test-Path $ini) {
        foreach ($line in Get-Content $ini) {
            $t = $line.Trim()
            if ($t -eq '' -or $t.StartsWith('#') -or $t.StartsWith(';')) { continue }
            $kv = $t -split '=', 2
            if ($kv.Count -ne 2) { continue }
            $k = $kv[0].Trim().ToLower(); $v = $kv[1].Trim().ToLower()
            $on = @('1', 'true', 'on', 'yes') -contains $v
            if ($k -eq 'tier11') { $tier = $on }
            if ($k -eq 'nowrap') { $nowrap = $on }
        }
    }
    $script:suppress = $true
    $chkTier.Checked = $tier; $chkNoWrap.Checked = $nowrap
    $script:suppress = $false
}

function Write-Ini {
    if ($script:suppress -or -not $script:targetDir) { return }
    $ini = Join-Path $script:targetDir 'dxr11.ini'
    $t = if ($chkTier.Checked) { '1' } else { '0' }
    $n = if ($chkNoWrap.Checked) { '1' } else { '0' }
    @(
        '; Written by dxr11-setup. Safe to edit by hand.',
        "tier11 = $t",
        "nowrap = $n"
    ) | Set-Content $ini -Encoding ascii
    $lblLogHint.Text = "Saved to $ini"
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
    foreach ($n in @('dxcompiler.dll', 'dxil.dll')) {
        $p = Find-Source $n
        if ($p) { Copy-Item $p (Join-Path $script:targetDir $n) -Force; $copied += ", $n" }
    }
    $lblLogHint.Text = "Copied $copied"
    Update-Status
})

$btnRemove.Add_Click({
    $answer = [Windows.Forms.MessageBox]::Show(
        "Delete d3d12.dll and dxr11.ini from`n$script:targetDir ?`n`nThis restores the original behaviour exactly. dxcompiler.dll and dxil.dll are left alone, since the game may use them itself.",
        'Remove the shim', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { return }
    foreach ($n in @('d3d12.dll', 'dxr11.ini')) {
        $p = Join-Path $script:targetDir $n
        if (Test-Path $p) { Remove-Item $p -Force }
    }
    $lblLogHint.Text = 'Removed.'
    Update-Status
})

$chkTier.Add_CheckedChanged({ Write-Ini })
$chkNoWrap.Add_CheckedChanged({ Write-Ini })

$btnLog.Add_Click({
    if (Test-Path $logPath) { Start-Process notepad.exe $logPath }
    else { $lblLogHint.Text = "No log yet at $logPath. Run the game once." }
})

$btnProblems.Add_Click({
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
