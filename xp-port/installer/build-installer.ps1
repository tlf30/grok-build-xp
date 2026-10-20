# Build the XP installer and stage a folder that can be copied to any XP SP3 machine.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# XP_BUILD is the host build directory (default %TEMP%\xp-rust-hello).
# XP_MSBUILD, XP_PAGER, and XP_FONT override the three inputs below.
$msbuild = if ($env:XP_MSBUILD) { $env:XP_MSBUILD } else { 'C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\MSBuild\15.0\Bin\MSBuild.exe' }
$buildRoot = if ($env:XP_BUILD) { $env:XP_BUILD } else { Join-Path $env:TEMP 'xp-rust-hello' }
$pager = if ($env:XP_PAGER) { $env:XP_PAGER } else { Join-Path $buildRoot 'target\i686-win7-windows-msvc\release\xai-grok-pager.exe' }
$font = if ($env:XP_FONT) { $env:XP_FONT } else { Join-Path $buildRoot 'fonts\CascadiaMono-install.ttf' }
if (-not (Test-Path -LiteralPath $msbuild)) { throw 'MSBuild is missing' }
if (-not (Test-Path -LiteralPath $pager)) { throw 'pager exe is missing' }
if (-not (Test-Path -LiteralPath $font)) { throw 'CascadiaMono font is missing' }

foreach ($proj in @('xp-setup.vcxproj', 'grok-launch.vcxproj', 'xp-screen.vcxproj')) {
    & $msbuild (Join-Path $here $proj) /p:Configuration=Release /p:Platform=Win32 /v:m
    if ($LASTEXITCODE -ne 0) { throw "build failed: $proj" }
}

$dist = Join-Path $here 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item -Force -LiteralPath (Join-Path $here 'xp-setup.exe') -Destination (Join-Path $dist 'xp-setup.exe')
Copy-Item -Force -LiteralPath (Join-Path $here 'grok-launch.exe') -Destination (Join-Path $dist 'grok-launch.exe')
Copy-Item -Force -LiteralPath (Join-Path $here 'xp-screen.exe') -Destination (Join-Path $dist 'xp-screen.exe')
Copy-Item -Force -LiteralPath $pager -Destination (Join-Path $dist 'xp-pager.exe')
python (Join-Path $here 'stamp-icon.py') (Join-Path $dist 'xp-pager.exe') (Join-Path $here 'grok.ico')
if ($LASTEXITCODE -ne 0) { throw 'could not stamp the pager icon' }
Copy-Item -Force -LiteralPath $font -Destination (Join-Path $dist 'CascadiaMono.ttf')
Get-ChildItem -LiteralPath $dist | ForEach-Object { '{0} {1}' -f $_.Length, $_.Name }
