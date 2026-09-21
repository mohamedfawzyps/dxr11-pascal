<#
    run_proxy_test.ps1 -Exe <path to sample exe> [-Seconds 9] [-OutDir <dir>]

    Runs a DXR sample twice, once with no proxy in its directory and once with
    our d3d12.dll dropped beside it, capturing a burst of window grabs in each
    run, then reports:
      - whether it survived, and its reported fps
      - the proxy log lines produced
      - the closest matching frame pair between the two runs

    Static samples (HelloWorld) should give an exactly matching pair. Animated
    ones (SimpleLighting rotates its camera and light on wall-clock time) will
    not line up in phase across two separate process launches, so the best pair
    is only ever close, not identical. Read it as "the proxy did not change what
    is drawn", not as a bit-exactness claim.
#>
param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [int]$Seconds = 9,
    [int]$IntervalMs = 1000,
    [switch]$Animated,
    [string]$OutDir = "$env:TEMP\dxr11_prooftest"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$helper = @'
using System; using System.Collections.Generic; using System.Drawing; using System.Runtime.InteropServices;
public class PxTest {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr d,int x,int y,int w,int hh,IntPtr s,int sx,int sy,int rop);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }

  public static IntPtr Find(uint pid, out string title) {
    IntPtr f = IntPtr.Zero; string t = "";
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) {
        var sb = new System.Text.StringBuilder(512); GetWindowTextW(h, sb, 512);
        if (sb.Length > 0) { f = h; t = sb.ToString(); return false; }
      }
      return true;
    }, IntPtr.Zero);
    title = t; return f;
  }

  // Grab the client area and return it sampled on an 8px grid as packed RGB.
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
  const uint SWP_NOMOVE = 0x2, SWP_NOSIZE = 0x1, SWP_SHOWWINDOW = 0x40;

  // These samples use a flip-model swapchain, whose contents are NOT in the DWM
  // redirection surface. PrintWindow and a BitBlt of an occluded window both come
  // back blank; the only thing that actually reads the rendered pixels is a BitBlt
  // while the window is genuinely visible on screen. SetForegroundWindow alone is
  // not enough either, because Windows silently ignores it when the calling
  // process is not itself foreground, which is exactly our case. So force topmost
  // as well, and settle before every grab rather than only the first.
  public static int[] Grab(IntPtr hwnd, string savePath, int settleMs) {
    RECT r; GetClientRect(hwnd, out r);
    int w = r.R-r.L, h = r.B-r.T;
    if (w <= 0 || h <= 0) return null;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    if (settleMs > 0) System.Threading.Thread.Sleep(settleMs);
    using (Bitmap bmp = new Bitmap(w, h)) {
      using (Graphics g = Graphics.FromImage(bmp)) {
        IntPtr dst = g.GetHdc(); IntPtr src = GetDC(hwnd);
        BitBlt(dst,0,0,w,h,src,0,0,0x00CC0020);
        ReleaseDC(hwnd, src); g.ReleaseHdc(dst);
      }
      if (savePath != null) bmp.Save(savePath, System.Drawing.Imaging.ImageFormat.Png);
      var list = new List<int>();
      for (int y = 0; y < h; y += 8) for (int x = 0; x < w; x += 8) list.Add(bmp.GetPixel(x,y).ToArgb() & 0xFFFFFF);
      return list.ToArray();
    }
  }

  // Max per-channel delta between two sampled grids, plus how many differ.
  public static int[] Compare(int[] a, int[] b) {
    int max = 0, n = 0;
    for (int i = 0; i < a.Length && i < b.Length; i++) {
      int d = Math.Max(Math.Abs(((a[i]>>16)&255)-((b[i]>>16)&255)),
              Math.Max(Math.Abs(((a[i]>>8)&255)-((b[i]>>8)&255)),
                       Math.Abs((a[i]&255)-(b[i]&255))));
      if (d > 0) { n++; if (d > max) max = d; }
    }
    return new int[] { n, max, a.Length };
  }
}
'@
if (-not ('PxTest' -as [type])) { Add-Type -TypeDefinition $helper -ReferencedAssemblies System.Drawing }

$dir = Split-Path -Parent $Exe
$proxySrc = Join-Path $PSScriptRoot "..\d3d12.dll"
$proxyDst = Join-Path $dir "d3d12.dll"
$log = Join-Path $env:TEMP "dxr11_proxy.log"
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

function Invoke-Run {
    param([string]$Label)
    if (Test-Path $log) { Clear-Content $log -ErrorAction SilentlyContinue }
    $p = Start-Process -FilePath $Exe -WorkingDirectory $dir -PassThru
    $grabs = @(); $titles = @(); $blank = 0
    $steps = [int](($Seconds * 1000) / $IntervalMs)
    $settle = [int](2500 / $IntervalMs)   # let the window appear before grabbing
    for ($i = 1; $i -le $steps; $i++) {
        Start-Sleep -Milliseconds $IntervalMs
        $p.Refresh()
        if ($p.HasExited) { Write-Host ("  {0}: EXITED at step {1}, code 0x{2:X8}" -f $Label,$i,$p.ExitCode); return $null }
        $t = ""; $h = [PxTest]::Find([uint32]$p.Id, [ref]$t)
        if ($h -ne 0 -and $i -ge $settle) {
            $g = [PxTest]::Grab($h, (Join-Path $OutDir ("{0}_t{1}.png" -f $Label,$i)), 350)
            # A flip-model swapchain that was not actually on screen grabs as a
            # near-flat image. Drop those rather than let them poison the diff.
            if ($g) {
                # Only a genuinely failed grab is near-solid. Do NOT set this
                # threshold high: a legitimate frame of a flat-shaded scene can
                # have as few as ~25 distinct colours across the sample grid.
                $variety = ($g | Select-Object -Unique).Count
                if ($variety -lt 4) { $blank++ } else { $grabs += ,$g; $titles += $t }
            }
        }
    }
    $alive = -not $p.HasExited
    if ($alive) { $p.Kill() }
    Start-Sleep -Milliseconds 400
    $fps = @($titles | ForEach-Object { if ($_ -match 'fps:\s*([\d.]+)') { [double]$Matches[1] } })
    $median = 0
    if ($fps.Count -gt 0) { $median = ($fps | Sort-Object)[[int]($fps.Count / 2)] }
    Write-Host ("  {0}: survived={1}  frames captured={2} (discarded {3} blank)  fps median={4:N0}" -f $Label,$alive,$grabs.Count,$blank,$median)
    $logText = ""
    if (Test-Path $log) { $logText = [IO.File]::ReadAllText($log) }
    return [pscustomobject]@{ Grabs = $grabs; Log = $logText }
}

Write-Host "=== $([IO.Path]::GetFileName($Exe)) ==="
if (Test-Path $proxyDst) { Remove-Item -LiteralPath $proxyDst -Force }
Write-Host "-- baseline (no proxy) --"
$base = Invoke-Run -Label "baseline"

Copy-Item -LiteralPath $proxySrc -Destination $proxyDst -Force
Write-Host "-- through proxy --"
$prox = Invoke-Run -Label "proxied"

if ($Animated) {
    Write-Host ""
    Write-Host "-Animated: skipping the frame comparison on purpose."
    Write-Host "  MEASURED, on D3D12RaytracingSimpleLighting: grabs taken seconds apart"
    Write-Host "  within one run come back byte-identical, even though the app reports"
    Write-Host "  ~1380 fps and its OnUpdate rotates the camera every frame. Across two"
    Write-Host "  runs, a constant 9609 of 14400 sampled pixels differ, at every"
    Write-Host "  timestep. So this capture path is not a trustworthy oracle for an"
    Write-Host "  animated flip-model swapchain, and we do not draw a conclusion from it."
    Write-Host "  NOT VERIFIED: why. Could be that BitBlt returns a stale frame for this"
    Write-Host "  swapchain, or that the sample's per-frame re-rotation of m_eye settles"
    Write-Host "  into a fixed view. Distinguishing the two needs a proper capture path"
    Write-Host "  (a hooked Present, or a UAV readback), not more window grabbing."
    Write-Host ""
    Write-Host "  Transparency evidence for this sample is therefore: it runs, fps"
    Write-Host "  matches baseline, and the proxy log shows interception. The"
    Write-Host "  pixel-exact claim comes from the static sample (HelloWorld) instead."
}
elseif ($base -and $prox -and $base.Grabs.Count -and $prox.Grabs.Count) {
    $bestN = [int]::MaxValue; $bestMax = 255; $total = 0
    foreach ($a in $base.Grabs) {
        foreach ($b in $prox.Grabs) {
            $c = [PxTest]::Compare($a, $b)
            if ($c[0] -lt $bestN) { $bestN = $c[0]; $bestMax = $c[1]; $total = $c[2] }
        }
    }
    Write-Host ""
    Write-Host ("closest frame pair: {0} of {1} sampled pixels differ, max channel delta {2}" -f $bestN,$total,$bestMax)
}
Write-Host ""
Write-Host "proxy log:"
Write-Host $prox.Log
