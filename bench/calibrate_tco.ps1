# calibrate_tco.ps1 - find this machine's heap-OOM depth threshold for a CEK
# interpreter (cppscheme2 or pyScheme).
#
# WHY: on these interpreters a broken tail call grows the heap-allocated
# continuation stack (KStack = std::vector<Frame> in cppscheme2; a list in
# pyScheme) and fails by running OUT OF MEMORY -- there is no fixed call-stack
# limit and no depth cap.  So the iteration count needed to expose broken TCO
# scales with RAM+pagefile and is machine-specific.  A deep NON-tail recursion
#   (define (grow k) (if (= k 0) 0 (+ 1 (grow (- k 1)))))   ; not a tail call
# grows the K-stack exactly like broken TCO would, so the N at which (grow N)
# dies IS the threshold.  Set -I: comfortably above it for a meaningful soak
# run on THIS machine.
#
# WARNING: trials near the threshold allocate many GB and may thrash the
# pagefile.  Run on an otherwise-idle machine.  Each trial is a FRESH process,
# so a failure just frees its memory -- nothing here corrupts state.
#
# USAGE:
#   pwsh bench/calibrate_tco.ps1                              # cppscheme2 (default)
#   pwsh bench/calibrate_tco.ps1 -Interp python,-m,pyscheme  # pyScheme
#   pwsh bench/calibrate_tco.ps1 -Start 200000 -MaxN 5e8 -TimeoutSec 180

param(
   [string[]] $Interp = @("D:/SWDEV/Languages/Lisp/4CPPScheme2/build/Release/cppscheme2.exe"),
   [string]   $InterpExe  = "",        # alt to -Interp: exe path (may contain spaces); used by cherry
   [string]   $InterpArgs = "",        # alt to -Interp: space-separated pre-args, e.g. "-m pyscheme"
   [long]     $Start  = 100000,        # first N to try (must comfortably succeed)
   [long]     $MaxN   = 200000000,     # hard safety cap on the ramp
   [int]      $TimeoutSec = 120,       # per-trial timeout; a slower trial counts as "too big"
   [string]   $Sentinel = "CALIBRATE_OK"
)

$ErrorActionPreference = "Stop"

# Resolve the interpreter invocation.  -InterpExe/-InterpArgs (used by cherry)
# take precedence over -Interp; they avoid array-quoting issues when the exe
# path contains spaces (e.g. a python.exe under "Program Files").
if ($InterpExe -ne "") {
   $ExePath = $InterpExe
   $PreArgs = if ($InterpArgs.Trim() -ne "") { $InterpArgs.Trim() -split '\s+' } else { @() }
} else {
   $ExePath = $Interp[0]
   $PreArgs = if ($Interp.Count -gt 1) { $Interp[1..($Interp.Count - 1)] } else { @() }
}
$base = [System.IO.Path]::GetTempFileName()
$scm  = "$base.scm"
$out  = "$base.out"
$err  = "$base.err"
Remove-Item $base -ErrorAction SilentlyContinue

function Test-N([long]$n) {
   $src = @"
(define (grow k) (if (= k 0) 0 (+ 1 (grow (- k 1)))))
(grow $n)
(display "$Sentinel") (newline)
"@
   Set-Content -Path $scm -Value $src -Encoding ASCII
   $argList = $PreArgs + @("`"$scm`"")
   $p = Start-Process -FilePath $ExePath -ArgumentList $argList -NoNewWindow -PassThru `
                      -RedirectStandardOutput $out -RedirectStandardError $err
   if (-not $p.WaitForExit($TimeoutSec * 1000)) {
      try { $p.Kill() } catch {}
      return $false      # too slow (thrashing) -> treat as too big
   }
   $text = (Get-Content $out -Raw -ErrorAction SilentlyContinue)
   return ($p.ExitCode -eq 0 -and $text -match $Sentinel)
}

try {
   Write-Host "Calibrating heap-OOM (broken-TCO) threshold for: $ExePath $($PreArgs -join ' ')"
   Write-Host "  (deep non-tail recursion proxy; Ctrl-C to abort)`n"

   # 1) exponential ramp: double N until the proxy fails (or we hit MaxN).
   [long]$lo = 0; [long]$hi = 0; [long]$n = $Start
   while ($true) {
      Write-Host ("  ramp   N = {0,15:N0} ..." -f $n) -NoNewline
      if (Test-N $n) {
         Write-Host " ok";  $lo = $n
         if ($n -ge $MaxN) { Write-Host "`nReached MaxN with no failure -- raise -MaxN."; Write-Output "CALIBRATE_THRESHOLD 0"; return }
         $n = [Math]::Min($n * 2, $MaxN)
      } else {
         Write-Host " FAILED (oom/crash/timeout)"; $hi = $n; break
      }
   }

   # 2) binary search between lo (last ok) and hi (first fail), ~2% precision.
   while (($hi - $lo) -gt [Math]::Max(1, [long]($lo / 50))) {
      [long]$mid = $lo + [long](($hi - $lo) / 2)
      Write-Host ("  bisect N = {0,15:N0} ..." -f $mid) -NoNewline
      if (Test-N $mid) { Write-Host " ok"; $lo = $mid } else { Write-Host " FAILED"; $hi = $mid }
   }

   Write-Host ""
   Write-Host ("  largest completed : {0,15:N0}" -f $lo)
   Write-Host ("  smallest failed   : {0,15:N0}" -f $hi)
   Write-Host ("  => broken TCO dies around ~{0:N0} iterations on this machine." -f $hi)
   Write-Host ("  => for a meaningful soak run, use -I: at or above ~{0:N0}." -f ([long]($hi * 1.5)))
   Write-Host "  (Approximate: depends on free memory and per-form frame size; re-run if RAM/pagefile changes.)"
   # Machine-readable result line (parsed by cherry's Test Suites dialog).
   Write-Output ("CALIBRATE_THRESHOLD {0}" -f $hi)
}
finally {
   Remove-Item $scm, $out, $err -ErrorAction SilentlyContinue
}
