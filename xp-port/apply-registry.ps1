# Re-apply the XP patches onto extracted crates.io sources, then drop the
# Cargo fingerprints so the next i686-win7 build actually recompiles them.
# `cargo --locked` cannot take a path patch that changes Cargo.lock.
# Checksums are checked at extract time, so a later copy is left in place.
# Run this again after `cargo clean` or a fresh extract of these crates.
$ErrorActionPreference = 'Stop'

# Patches live next to this script. The cargo target is $env:XP_BUILD\target,
# or %TEMP%\xp-rust-hello\target when XP_BUILD is unset.
$portRoot = Join-Path $PSScriptRoot 'registry'
$registrySrc = Join-Path $env:USERPROFILE '.cargo\registry\src'
$buildRoot = if ($env:XP_BUILD) { $env:XP_BUILD } else { Join-Path $env:TEMP 'xp-rust-hello' }
$targetRoot = Join-Path $buildRoot 'target'

$crates = @(
    @{
        Name = 'mio-1.2.1'
        Files = @(
            'src\sys\windows\net.rs',
            'src\sys\windows\selector.rs'
        )
    },
    @{
        Name = 'socket2-0.6.4'
        Files = @('src\sys\windows.rs')
    },
    @{
        Name = 'cc-1.2.48'
        Files = @('src\tempfile.rs')
    },
    @{
        Name = 'crossterm-0.28.1'
        Files = @(
            'src\style.rs',
            'src\style\sys\windows.rs',
            'src\terminal.rs',
            'src\event\sys\windows\parse.rs'
        )
    }
)

if (-not (Test-Path -LiteralPath $registrySrc)) {
    Write-Output "NO_REGISTRY $registrySrc"
    exit 2
}

$copied = 0
$indexes = @(Get-ChildItem -LiteralPath $registrySrc -Directory)
foreach ($index in $indexes) {
    foreach ($crate in $crates) {
        $destDir = Join-Path $index.FullName $crate.Name
        if (-not (Test-Path -LiteralPath $destDir)) { continue }
        foreach ($rel in $crate.Files) {
            $from = Join-Path (Join-Path $portRoot $crate.Name) $rel
            $to = Join-Path $destDir $rel
            if (-not (Test-Path -LiteralPath $from)) { throw "missing saved patch $from" }
            if (-not (Test-Path -LiteralPath $to)) { throw "missing extracted file $to" }
            Copy-Item -LiteralPath $from -Destination $to -Force
            $fromHash = (Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash
            $toHash = (Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash
            if ($fromHash -ne $toHash) { throw "hash mismatch after copy $rel" }
            $copied++
            Write-Output "copied $($crate.Name) $rel"
        }
    }
}

$deleted = 0
$samples = New-Object System.Collections.Generic.List[string]
if (Test-Path -LiteralPath $targetRoot) {
    foreach ($triple in @(Get-ChildItem -LiteralPath $targetRoot -Directory)) {
        foreach ($profile in @('release', 'debug')) {
            $base = Join-Path $triple.FullName $profile
            $fp = Join-Path $base '.fingerprint'
            if (Test-Path -LiteralPath $fp) {
                foreach ($dir in @(Get-ChildItem -LiteralPath $fp -Directory)) {
                    if ($samples.Count -lt 12) { $samples.Add($dir.Name) }
                    if ($dir.Name -match '^(mio|socket2|cc|crossterm)-[0-9a-f]{16}$') {
                        Remove-Item -LiteralPath $dir.FullName -Recurse -Force
                        $deleted++
                        Write-Output "rm fingerprint $($dir.Name)"
                    }
                }
            }
            $deps = Join-Path $base 'deps'
            if (Test-Path -LiteralPath $deps) {
                foreach ($file in @(Get-ChildItem -LiteralPath $deps -File)) {
                    if ($file.Name -match '^(lib)?(mio|socket2|cc|crossterm)-[0-9a-f]+\.(rlib|rmeta|d)$') {
                        Remove-Item -LiteralPath $file.FullName -Force
                        $deleted++
                        Write-Output "rm dep $($file.Name)"
                    }
                }
            }
        }
    }
}

Write-Output "copied=$copied deleted=$deleted"
if ($copied -eq 0) {
    Write-Output 'NO_EXTRACT'
    exit 2
}
if ($deleted -eq 0) {
    Write-Output 'fingerprint samples (none matched mio/socket2/cc):'
    foreach ($name in $samples) { Write-Output "sample $name" }
}
exit 0
