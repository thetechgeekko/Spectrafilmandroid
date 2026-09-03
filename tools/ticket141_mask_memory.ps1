param(
    [Parameter(Mandatory = $true)] [string] $TargetApk,
    [Parameter(Mandatory = $true)] [string] $TestApk,
    [string] $Serial = "",
    [string] $EvidenceDirectory = "build/evidence/ticket141",
    [string] $DurableEvidenceDirectory = "docs/evidence/ticket141/current",
    [int] $MaskCount = 4,
    [int] $Repeats = 2
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$target = (Resolve-Path $TargetApk).Path
$test = (Resolve-Path $TestApk).Path
$sourceInputs = @(
    "app/src/main/java/com/spectrafilm/app/masks/Mask.kt"
    "app/src/main/java/com/spectrafilm/app/masks/MaskCompositor.kt"
    "app/src/main/java/com/spectrafilm/app/masks/MaskRaster.kt"
    "app/src/main/java/com/spectrafilm/app/masks/MaskSpatial.kt"
    "app/src/main/java/com/spectrafilm/app/masks/MaskTileScratch.kt"
    "app/src/main/java/com/spectrafilm/app/EngineHelpers.kt"
    "app/src/main/java/com/spectrafilm/app/AppMemoryBudget.kt"
    "app/src/test/java/com/spectrafilm/app/masks/MaskTiledCompositorTest.kt"
    "app/src/androidTest/java/com/spectrafilm/app/Ticket141MaskMemoryChecks.kt"
    "app/src/androidTest/java/com/spectrafilm/app/ReleaseCandidateSmokeInstrumentation.java"
    "app/src/main/AndroidManifest.xml"
    "app/src/androidTest/AndroidManifest.xml"
    "app/build.gradle.kts"
    "app/proguard-rules.pro"
    "app/gradle.lockfile"
    "engine/spektra-core/src/main/kotlin/com/spectrafilm/engine/SpektraEngine.kt"
    "engine/spektra-core/build.gradle.kts"
    "engine/spektra-core/gradle.lockfile"
    "build.gradle.kts"
    "settings.gradle.kts"
    "gradle.properties"
    "gradle/libs.versions.toml"
    "gradle/wrapper/gradle-wrapper.properties"
    "gradle/wrapper/gradle-wrapper.jar"
    "gradlew.bat"
    "debug.keystore"
    "tools/ticket141_mask_memory.ps1"
    "docs/MASKING_SPEC.md"
    "docs/MASK_COMPOSITOR_MEMORY.md"
) | Sort-Object -Unique

function Get-SourceManifestLines {
    $lines = foreach ($relativePath in $sourceInputs) {
        $absolutePath = Join-Path $repoRoot $relativePath
        if (-not (Test-Path -LiteralPath $absolutePath -PathType Leaf)) {
            throw "ticket #141 source/build input is missing: $relativePath"
        }
        $hash = (Get-FileHash -LiteralPath $absolutePath -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $($relativePath.Replace('\', '/'))"
    }
    return @($lines)
}

$evidenceRoot = Join-Path $repoRoot $EvidenceDirectory
$durableEvidenceRoot = Join-Path $repoRoot $DurableEvidenceDirectory
New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
New-Item -ItemType Directory -Force -Path $durableEvidenceRoot | Out-Null

function Write-EvidenceText {
    param(
        [Parameter(Mandatory = $true)] [string] $Name,
        [Parameter(Mandatory = $true)] $Value
    )
    Set-Content -LiteralPath (Join-Path $evidenceRoot $Name) -Value $Value -Encoding utf8
    Set-Content -LiteralPath (Join-Path $durableEvidenceRoot $Name) -Value $Value -Encoding utf8
}

Write-EvidenceText "qualification_status.txt" @(
    "status=INCOMPLETE"
    "reason=device workflow has not completed and source stability has not been rechecked"
)

$runnerSource = Join-Path $repoRoot "app/src/androidTest/java/com/spectrafilm/app/ReleaseCandidateSmokeInstrumentation.java"
$runnerText = Get-Content -LiteralPath $runnerSource -Raw
if (-not $runnerText.Contains("ticket141_width")) {
    throw "Ticket #141 runner integration is not present. Root must wire Ticket141MaskMemoryChecks after the shared #139 runner edit lands."
}

$adbCommand = Get-Command adb -ErrorAction SilentlyContinue
if ($null -eq $adbCommand) {
    $sdkAdb = Join-Path $env:LOCALAPPDATA "Android/Sdk/platform-tools/adb.exe"
    if (-not (Test-Path -LiteralPath $sdkAdb)) { throw "adb was not found" }
    $adb = $sdkAdb
} else {
    $adb = $adbCommand.Source
}
$adbArgs = @()
if ($Serial.Length -gt 0) { $adbArgs += @("-s", $Serial) }

& $adb @adbArgs get-state | Out-Null
if ($LASTEXITCODE -ne 0) { throw "the requested Android device is not online" }
$targetHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
$testHash = (Get-FileHash -LiteralPath $test -Algorithm SHA256).Hash.ToLowerInvariant()
$headCommit = (& git -C $repoRoot rev-parse HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $headCommit.Length -eq 0) { throw "could not resolve the source commit" }
$headTree = (& git -C $repoRoot rev-parse 'HEAD^{tree}' | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $headTree.Length -eq 0) { throw "could not resolve the source tree" }
$sourceStatus = @(& git -C $repoRoot status --porcelain=v1 --untracked-files=all -- @sourceInputs)
if ($LASTEXITCODE -ne 0) { throw "could not record ticket #141 source status" }
$sourceManifest = @(Get-SourceManifestLines)
Write-EvidenceText "source_manifest.sha256" $sourceManifest
Write-EvidenceText "source_status.txt" $(if ($sourceStatus.Count -eq 0) { "listed_inputs_clean=true" } else { $sourceStatus })
$sourceManifestHash = (Get-FileHash -LiteralPath (Join-Path $evidenceRoot "source_manifest.sha256") -Algorithm SHA256).Hash.ToLowerInvariant()
$artifactManifest = @(
    "$targetHash  supplied-target.apk"
    "$testHash  supplied-test.apk"
)
Write-EvidenceText "artifact_manifest.sha256" $artifactManifest
$deviceSerial = (& $adb @adbArgs get-serialno | Out-String).Trim()
$provenance = @(
    "captured_utc=$([DateTime]::UtcNow.ToString('o'))"
    "source_commit=$headCommit"
    "source_tree=$headTree"
    "source_manifest_sha256=$sourceManifestHash"
    "mask_count=$MaskCount"
    "repeats=$Repeats"
    "cells=4096x3052,8192x6104"
    "serial=$deviceSerial"
    "manufacturer=$((& $adb @adbArgs shell getprop ro.product.manufacturer | Out-String).Trim())"
    "model=$((& $adb @adbArgs shell getprop ro.product.model | Out-String).Trim())"
    "api=$((& $adb @adbArgs shell getprop ro.build.version.sdk | Out-String).Trim())"
    "fingerprint=$((& $adb @adbArgs shell getprop ro.build.fingerprint | Out-String).Trim())"
    "target_sha256=$targetHash"
    "test_sha256=$testHash"
)
Write-EvidenceText "provenance.txt" $provenance

& $adb @adbArgs install --no-streaming -r $target
if ($LASTEXITCODE -ne 0) { throw "failed to install the exact target APK" }
& $adb @adbArgs install --no-streaming -r $test
if ($LASTEXITCODE -ne 0) { throw "failed to install the exact test APK" }

$installedPackages = @(
    @{ Name = "target"; Package = "com.spectrafilm.app"; Hash = $targetHash },
    @{ Name = "test"; Package = "com.spectrafilm.app.test"; Hash = $testHash }
)
foreach ($installed in $installedPackages) {
    $remotePaths = @(
        @(& $adb @adbArgs shell pm path $installed.Package) |
            ForEach-Object { $_.ToString().Trim() } |
            Where-Object { $_ -match '^package:.*/base\.apk$' }
    )
    if ($remotePaths.Count -ne 1) {
        throw "$($installed.Package) did not expose exactly one installed base APK"
    }
    $remote = $remotePaths[0].Substring("package:".Length)
    $pulled = Join-Path $evidenceRoot ("installed-{0}-base.apk" -f $installed.Name)
    & $adb @adbArgs pull $remote $pulled | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "failed to preserve installed $($installed.Name) APK" }
    $installedHash = (Get-FileHash -LiteralPath $pulled -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($installedHash -ne $installed.Hash) {
        throw "installed $($installed.Name) APK hash differs from the supplied exact artifact"
    }
}

$runner = "com.spectrafilm.app.test/com.spectrafilm.app.ReleaseCandidateSmokeInstrumentation"
$cells = @(
    @{ Name = "12_5mp"; Width = 4096; Height = 3052 },
    @{ Name = "50mp"; Width = 8192; Height = 6104 }
)

$denialOutput = & $adb @adbArgs shell am instrument -w `
    -e ticket141_width 4096 `
    -e ticket141_height 3052 `
    -e ticket141_masks $MaskCount `
    -e ticket141_force_denial true `
    $runner 2>&1
$denialText = $denialOutput -join "`n"
$denialPath = Join-Path $evidenceRoot "mask_budget_denial.txt"
Write-EvidenceText "mask_budget_denial.txt" $denialText
if ($LASTEXITCODE -ne 0 -or -not $denialText.Contains("TICKET141_MASK_DENIAL: PASS")) {
    throw "ticket #141 graceful-denial cell failed; see $denialPath"
}

foreach ($cell in $cells) {
    $output = & $adb @adbArgs shell am instrument -w `
        -e ticket141_width $cell.Width `
        -e ticket141_height $cell.Height `
        -e ticket141_masks $MaskCount `
        -e ticket141_repeats $Repeats `
        $runner 2>&1
    $text = $output -join "`n"
    $path = Join-Path $evidenceRoot ("mask_memory_{0}.txt" -f $cell.Name)
    Write-EvidenceText ("mask_memory_{0}.txt" -f $cell.Name) $text
    if ($LASTEXITCODE -ne 0 -or -not $text.Contains("TICKET141_MASK_MEMORY: RESULT")) {
        throw "ticket #141 $($cell.Name) device cell failed; see $path"
    }
}

$sourceManifestAfter = @(Get-SourceManifestLines)
$sourceChanges = @(Compare-Object -ReferenceObject $sourceManifest -DifferenceObject $sourceManifestAfter)
if ($sourceChanges.Count -ne 0) {
    Write-EvidenceText "source_manifest_after.sha256" $sourceManifestAfter
    throw "ticket #141 source/build inputs changed during the device run; evidence is not authoritative"
}
$headCommitAfter = (& git -C $repoRoot rev-parse HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $headCommitAfter -ne $headCommit) {
    throw "repository HEAD changed during the device run; evidence is not authoritative"
}

$thermalAfter = & $adb @adbArgs shell dumpsys thermalservice 2>&1
Write-EvidenceText "thermal_after.txt" $thermalAfter
$batteryAfter = & $adb @adbArgs shell dumpsys battery 2>&1
Write-EvidenceText "battery_after.txt" $batteryAfter
Write-EvidenceText "qualification_status.txt" @(
    "status=COMPLETE"
    "source_commit=$headCommit"
    "source_manifest_sha256=$sourceManifestHash"
    "target_sha256=$targetHash"
    "test_sha256=$testHash"
)

Write-Output "Ticket #141 raw device evidence captured at $evidenceRoot"
Write-Output "Ticket #141 durable textual evidence captured at $durableEvidenceRoot"
Write-Output "This script does not declare closure: review PSS/RSS, digests, thermal state, exact APK hashes, and runner provenance."
