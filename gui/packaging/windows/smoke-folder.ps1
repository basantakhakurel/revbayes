<#
.SYNOPSIS
  Smoke-test a DEPLOYED RevStudio folder (GUI_Implementation_Note.md, sections 9.7 and 10.5).

.DESCRIPTION
  Runs `RevStudio.exe --selftest` with PATH reduced to the Windows directories, so nothing from the runner's Qt
  installation can hide a DLL or plugin that the folder is missing.

  RevStudio.exe is a GUI-subsystem program: launching it from a shell returns immediately and does not set
  $LASTEXITCODE. So it is started with Start-Process -Wait -PassThru, and the outcome is read from the report file the
  program writes itself (--report), never from captured stdout.

  STATUS: written in phase 0 and NOT yet run on Windows.

.PARAMETER Backend
  A backend to talk to: mock-rb.exe in gui-build.yml (no rb is available there), the real rb.exe in the bundle job.

.PARAMETER CopyBackendIntoFolder
  Copy the backend into the folder for the test and remove it afterwards. Needed for mock-rb.exe, which links Qt
  and so needs the folder's Qt DLLs; the real rb.exe needs no such thing.
#>
param(
  [Parameter(Mandatory)] [string] $Folder,
  [Parameter(Mandatory)] [string] $Backend,
  [switch] $CopyBackendIntoFolder
)
$ErrorActionPreference = "Stop"

$folder = (Resolve-Path $Folder).Path
$exe = Join-Path $folder "RevStudio.exe"
$backendPath = (Resolve-Path $Backend).Path
$copied = $null

try {
  if ($CopyBackendIntoFolder) {
    $copied = Join-Path $folder (Split-Path $backendPath -Leaf)
    Copy-Item $backendPath $copied
    $backendPath = $copied
  }

  # Only the Windows directories; drop the variables install-qt-action added.
  $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
  foreach ($name in "QT_PLUGIN_PATH", "QT_ROOT_DIR", "Qt6_DIR", "QML2_IMPORT_PATH") {
    Remove-Item "Env:$name" -ErrorAction SilentlyContinue
  }

  $report = Join-Path ([System.IO.Path]::GetTempPath()) "revstudio-selftest.txt"
  Remove-Item $report -ErrorAction SilentlyContinue

  $process = Start-Process -FilePath $exe -Wait -PassThru -NoNewWindow `
             -ArgumentList @("--selftest", "--rb", "`"$backendPath`"", "--report", "`"$report`"")

  if (Test-Path $report) { Get-Content $report | Write-Host } else { Write-Host "(no report was written)" }
  if ($process.ExitCode -ne 0) { throw "self-test failed with exit code $($process.ExitCode)" }
  Write-Host "smoke test passed"
}
finally {
  if ($copied) { Remove-Item $copied -ErrorAction SilentlyContinue }
}
