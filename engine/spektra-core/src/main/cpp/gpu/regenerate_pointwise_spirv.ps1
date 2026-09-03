# SPDX-FileCopyrightText: 2026 Spektrafilm Android contributors
# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [string]$NdkRoot
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($NdkRoot)) {
    if ([string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) {
        throw 'Pass -NdkRoot or set ANDROID_HOME.'
    }
    $NdkRoot = Join-Path $env:ANDROID_HOME 'ndk\27.0.12077973'
}

$glslc = Join-Path $NdkRoot 'shader-tools\windows-x86_64\glslc.exe'
if (-not (Test-Path -LiteralPath $glslc -PathType Leaf)) {
    throw "Pinned NDK glslc not found: $glslc"
}

$pairs = @(
    @('filming.comp', 'filming_spv.inc'),
    @('printing.comp', 'printing_spv.inc'),
    @('scan_spectral_chain.comp', 'scan_spectral_chain_spv.inc')
)
foreach ($pair in $pairs) {
    $source = Join-Path $PSScriptRoot $pair[0]
    $output = Join-Path $PSScriptRoot $pair[1]
    & $glslc -fshader-stage=compute --target-env=vulkan1.1 -mfmt=c `
        -o $output $source
    if ($LASTEXITCODE -ne 0) {
        throw "glslc failed for $($pair[0]) with exit code $LASTEXITCODE"
    }
}

foreach ($pair in $pairs) {
    foreach ($name in $pair) {
        $path = Join-Path $PSScriptRoot $name
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        Write-Output "$hash  $name"
    }
}
