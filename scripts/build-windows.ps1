param(
  [string]$WebRTC = (Join-Path $PSScriptRoot '..\..\webrtc'),
  [string]$VcpkgRoot = $env:VCPKG_ROOT,
  [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
if (!$VcpkgRoot) { throw 'Set VCPKG_ROOT or pass -VcpkgRoot (e.g. D:\vcpkg).' }
$env:VCPKG_ROOT = (Resolve-Path -LiteralPath $VcpkgRoot).Path
$nasm = Get-ChildItem -LiteralPath (Join-Path $VcpkgRoot 'downloads\tools\nasm') -Filter nasm.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if ($nasm) { $env:PATH = $nasm.Directory.FullName + ';' + $env:PATH }
$git = Get-Command git -ErrorAction Stop
$gitUsr = Join-Path (Split-Path (Split-Path $git.Source)) 'usr\bin'
if (Test-Path (Join-Path $gitUsr 'perl.exe')) { $env:PATH = $gitUsr + ';' + $env:PATH }
Push-Location (Join-Path $PSScriptRoot '..')
try {
  cmake --preset windows-x64 "-DWEBRTC_SOURCE_DIR=$WebRTC"
  if ($LASTEXITCODE) { throw 'CMake configure failed' }
  cmake --build out/windows-x64 --config $Configuration --parallel 3
  if ($LASTEXITCODE) { throw 'Build failed' }
  ctest --test-dir out/windows-x64 -C $Configuration --output-on-failure
  if ($LASTEXITCODE) { throw 'Tests failed' }
} finally { Pop-Location }
