#Requires -Version 5.1

[CmdletBinding(DefaultParameterSetName = 'Artifacts')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Artifacts')]
    [ValidateNotNullOrEmpty()]
    [string]$AppApk,

    [Parameter(Mandatory = $true, ParameterSetName = 'Artifacts')]
    [ValidateNotNullOrEmpty()]
    [string]$TestApk,

    [Parameter(Mandatory = $true, ParameterSetName = 'Build')]
    [switch]$Build,

    [string]$DeviceSerial,
    [string]$EvidenceDir,
    [string]$SdkRoot,
    [string]$BuildToolsVersion = '35.0.0'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$TargetPackage = 'com.spectrafilm.app'
$TestPackage = 'com.spectrafilm.app.test'
$Runner = 'com.spectrafilm.app.test/com.spectrafilm.app.ReleaseCandidateSmokeInstrumentation'
$script:AdbPath = $null
$script:SelectedDevice = $null
$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $nativeOutput = @()
    $exitCode = $null
    Remove-Variable LASTEXITCODE -Scope Local -ErrorAction SilentlyContinue
    $global:LASTEXITCODE = $null
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5.1 can promote a native process's stderr to an
        # ErrorRecord. Keep collecting it so the real native exit code remains
        # the fail-closed authority, then restore the caller's preference.
        $ErrorActionPreference = 'Continue'
        $nativeOutput = @(& $FilePath @Arguments 2>&1)
        $exitCode = $global:LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    $text = ($nativeOutput | ForEach-Object { $_.ToString() }) -join "`n"
    if ($null -eq $exitCode) {
        throw "$Label did not report a native exit code:`n$text"
    }
    if ($exitCode -ne 0) {
        throw "$Label failed with exit code ${exitCode}:`n$text"
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $text
    }
}

function Invoke-Adb {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $adbArguments = @()
    if (-not [string]::IsNullOrWhiteSpace($script:SelectedDevice)) {
        $adbArguments += @('-s', $script:SelectedDevice)
    }
    $adbArguments += $Arguments
    return Invoke-Native -FilePath $script:AdbPath -Arguments $adbArguments -Label 'adb'
}

function Require-File {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label is missing: $Path"
    }
    return (Get-Item -LiteralPath $Path).FullName
}

function Remove-ExistingOutput {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path) {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "refusing to replace a non-file output: $Path"
        }
        Remove-Item -LiteralPath $Path -Force
    }
}

function Get-SingleSignerSha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Apk,
        [Parameter(Mandatory = $true)][string]$ApkSigner
    )

    $verification = Invoke-Native -FilePath $ApkSigner `
        -Arguments @('verify', '--verbose', '--print-certs', $Apk) `
        -Label "signature verification for $Apk"
    $pattern = '(?im)^Signer #[0-9]+ certificate SHA-256 digest:\s*([0-9a-f:]+)\s*$'
    $matches = [regex]::Matches($verification.Output, $pattern)
    if ($matches.Count -ne 1) {
        throw "$Apk must contain exactly one signer; found $($matches.Count)"
    }
    $digest = $matches[0].Groups[1].Value.Replace(':', '').ToLowerInvariant()
    if ($digest -notmatch '^[0-9a-f]{64}$') {
        throw "$Apk signer SHA-256 digest is malformed"
    }
    return $digest
}

function Get-InstalledBasePath {
    param([Parameter(Mandatory = $true)][string]$PackageName)

    $result = Invoke-Adb -Arguments @('shell', 'pm', 'path', $PackageName)
    $paths = @()
    foreach ($line in ($result.Output -split "`r?`n")) {
        if ($line -match '^package:(.+)$') {
            $paths += $Matches[1].Trim()
        }
    }
    if ($paths.Count -eq 0) {
        return $null
    }
    $basePaths = @($paths | Where-Object { $_ -match '/base\.apk$' })
    if ($basePaths.Count -ne 1) {
        throw "$PackageName did not expose exactly one installed base APK"
    }
    return $basePaths[0]
}

function Pull-InstalledBase {
    param(
        [Parameter(Mandatory = $true)][string]$PackageName,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    Remove-ExistingOutput $Destination
    $remotePath = Get-InstalledBasePath -PackageName $PackageName
    if ([string]::IsNullOrWhiteSpace($remotePath)) {
        throw "$PackageName is not installed"
    }
    $null = Invoke-Adb -Arguments @('pull', $remotePath, $Destination)
    return Require-File -Path $Destination -Label "pulled $PackageName base APK"
}

function Assert-InstalledSignerCompatible {
    param(
        [Parameter(Mandatory = $true)][string]$PackageName,
        [Parameter(Mandatory = $true)][string]$ExpectedSigner,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ApkSigner
    )

    Remove-ExistingOutput $Destination
    $remotePath = Get-InstalledBasePath -PackageName $PackageName
    if ([string]::IsNullOrWhiteSpace($remotePath)) {
        Write-Host "$PackageName is not currently installed; signer preflight is not needed."
        return
    }
    $null = Invoke-Adb -Arguments @('pull', $remotePath, $Destination)
    $pulled = Require-File -Path $Destination -Label "pre-install $PackageName base APK"
    $actualSigner = Get-SingleSignerSha256 -Apk $pulled -ApkSigner $ApkSigner
    if ($actualSigner -ne $ExpectedSigner) {
        throw "$PackageName already uses signer $actualSigner, not candidate signer $ExpectedSigner; replace-only install refused"
    }
}

function Assert-InstallSucceeded {
    param(
        [Parameter(Mandatory = $true)]$Result,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Result.Output -notmatch '(?m)^Success\s*$') {
        throw "$Label did not report Success:`n$($Result.Output)"
    }
}

function Save-Evidence {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [AllowEmptyString()][string]$Text
    )

    [System.IO.File]::WriteAllText($Path, $Text + "`n", $script:Utf8NoBom)
}

function Assert-OutputLine {
    param(
        [Parameter(Mandatory = $true)][string]$Output,
        [Parameter(Mandatory = $true)][string]$ExpectedLine,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $lines = @($Output -split "`r?`n")
    if ($lines -notcontains $ExpectedLine) {
        throw "$Label is missing exact line '$ExpectedLine':`n$Output"
    }
}

function Assert-InstrumentationOutput {
    param(
        [Parameter(Mandatory = $true)][string]$instrumentOutput,
        [Parameter(Mandatory = $true)][string[]]$RequiredMarkers,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $failurePattern = '(?im)^(?:[^\r\n]*:\s*FAIL(?:\s|$)|INSTRUMENTATION_FAILED:|FAILURES!!!)'
    if ($instrumentOutput -match $failurePattern) {
        throw "$Label instrumentation output contains FAIL:`n$instrumentOutput"
    }
    foreach ($marker in $RequiredMarkers) {
        if ($instrumentOutput.IndexOf($marker, [System.StringComparison]::Ordinal) -lt 0) {
            throw "$Label instrumentation output is missing '$marker':`n$instrumentOutput"
        }
    }
    Assert-OutputLine $instrumentOutput 'INSTRUMENTATION_CODE: -1' $Label
}

function Invoke-Instrumentation {
    param(
        [string[]]$ExtraArguments = @(),
        [Parameter(Mandatory = $true)][string]$EvidencePath
    )

    $arguments = @('shell', 'am', 'instrument', '-w', '-r')
    $arguments += $ExtraArguments
    $arguments += $Runner
    Remove-ExistingOutput $EvidencePath
    $result = Invoke-Adb -Arguments $arguments
    Save-Evidence -Path $EvidencePath -Text $result.Output
    return $result.Output
}

function Get-ProbeToken {
    param(
        [Parameter(Mandatory = $true)][string]$Output,
        [Parameter(Mandatory = $true)][string]$Prefix
    )

    $matches = [regex]::Matches($Output, [regex]::Escape($Prefix) + '([^\s\r\n]+)')
    if ($matches.Count -ne 1) {
        throw "expected exactly one token after '$Prefix'"
    }
    return $matches[0].Groups[1].Value
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if ([string]::IsNullOrWhiteSpace($EvidenceDir)) {
    $EvidenceDir = Join-Path $repoRoot 'build\release-device-gate'
}
$null = New-Item -ItemType Directory -Force -Path $EvidenceDir
$EvidenceDir = (Get-Item -LiteralPath $EvidenceDir).FullName
# A reused evidence directory must never retain an earlier authoritative PASS
# when this run exits before producing its own complete summary.
Remove-ExistingOutput (Join-Path $EvidenceDir 'summary.txt')

if ([string]::IsNullOrWhiteSpace($SdkRoot)) {
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) {
        $SdkRoot = $env:ANDROID_SDK_ROOT
    } elseif (-not [string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) {
        $SdkRoot = $env:ANDROID_HOME
    } elseif (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $SdkRoot = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
    } else {
        throw 'Android SDK root was not provided and no standard environment location exists'
    }
}
$SdkRoot = (Get-Item -LiteralPath $SdkRoot).FullName
$script:AdbPath = Require-File -Path (Join-Path $SdkRoot 'platform-tools\adb.exe') -Label 'adb'
$buildTools = Join-Path $SdkRoot ("build-tools\" + $BuildToolsVersion)
$apkSigner = Require-File -Path (Join-Path $buildTools 'apksigner.bat') -Label 'apksigner'
$zipAlign = Require-File -Path (Join-Path $buildTools 'zipalign.exe') -Label 'zipalign'

if ($PSCmdlet.ParameterSetName -eq 'Build') {
    $gradle = Require-File -Path (Join-Path $repoRoot 'gradlew.bat') -Label 'Gradle wrapper'
    $null = Invoke-Native -FilePath $gradle `
        -Arguments @('--offline', '--no-daemon', ':app:assembleRelease', ':app:assembleReleaseAndroidTest') `
        -Label 'offline release build'

    $sourceAppName = 'app-release-unsigned.apk'
    if (Test-Path -LiteralPath (Join-Path $repoRoot 'keystore.properties') -PathType Leaf) {
        $sourceAppName = 'app-release.apk'
    }
    $sourceApp = Require-File `
        -Path (Join-Path $repoRoot ("app\build\outputs\apk\release\" + $sourceAppName)) `
        -Label 'built release app APK'
    $sourceTest = Require-File `
        -Path (Join-Path $repoRoot 'app\build\outputs\apk\androidTest\release\app-release-androidTest.apk') `
        -Label 'built release instrumentation APK'
    $debugKey = Require-File -Path (Join-Path $repoRoot 'debug.keystore') -Label 'repository debug keystore'
    $alignedApp = Join-Path $EvidenceDir 'local-app-aligned.apk'
    $alignedTest = Join-Path $EvidenceDir 'local-test-aligned.apk'
    $AppApk = Join-Path $EvidenceDir 'local-app-signed.apk'
    $TestApk = Join-Path $EvidenceDir 'local-test-signed.apk'

    $null = Invoke-Native -FilePath $zipAlign -Arguments @('-f', '-P', '16', '4', $sourceApp, $alignedApp) -Label 'app zip alignment'
    $null = Invoke-Native -FilePath $zipAlign -Arguments @('-f', '-P', '16', '4', $sourceTest, $alignedTest) -Label 'test zip alignment'
    $signing = @('--ks', $debugKey, '--ks-key-alias', 'androiddebugkey', '--ks-pass', 'pass:android', '--key-pass', 'pass:android')
    $null = Invoke-Native -FilePath $apkSigner -Arguments (@('sign') + $signing + @('--out', $AppApk, $alignedApp)) -Label 'local app signing'
    $null = Invoke-Native -FilePath $apkSigner -Arguments (@('sign') + $signing + @('--out', $TestApk, $alignedTest)) -Label 'local test signing'
    $null = Invoke-Native -FilePath $zipAlign -Arguments @('-c', '-v', '-P', '16', '4', $AppApk) -Label 'signed app alignment check'
    $null = Invoke-Native -FilePath $zipAlign -Arguments @('-c', '-v', '-P', '16', '4', $TestApk) -Label 'signed test alignment check'
}

$AppApk = Require-File -Path $AppApk -Label 'signed app APK'
$TestApk = Require-File -Path $TestApk -Label 'signed instrumentation APK'
$candidateAppSigner = Get-SingleSignerSha256 -Apk $AppApk -ApkSigner $apkSigner
$candidateTestSigner = Get-SingleSignerSha256 -Apk $TestApk -ApkSigner $apkSigner
if ($candidateAppSigner -ne $candidateTestSigner) {
    throw 'candidate app and test APK signers differ'
}

$devices = Invoke-Native -FilePath $script:AdbPath -Arguments @('devices') -Label 'adb device discovery'
$readyDevices = @()
foreach ($match in [regex]::Matches($devices.Output, '(?m)^([^\s]+)\s+device\s*$')) {
    $readyDevices += $match.Groups[1].Value
}
if ([string]::IsNullOrWhiteSpace($DeviceSerial)) {
    if ($readyDevices.Count -ne 1) {
        throw "expected exactly one ready Android device; found $($readyDevices.Count)"
    }
    $script:SelectedDevice = $readyDevices[0]
} else {
    if ($readyDevices -notcontains $DeviceSerial) {
        throw "requested device is not ready: $DeviceSerial"
    }
    $script:SelectedDevice = $DeviceSerial
}

$preTarget = Join-Path $EvidenceDir 'preinstall-target-base.apk'
$preTest = Join-Path $EvidenceDir 'preinstall-test-base.apk'
Assert-InstalledSignerCompatible $TargetPackage $candidateAppSigner $preTarget $apkSigner
Assert-InstalledSignerCompatible $TestPackage $candidateTestSigner $preTest $apkSigner

$appInstall = Invoke-Adb -Arguments @('install', '--no-streaming', '-r', $AppApk)
Assert-InstallSucceeded -Result $appInstall -Label 'target replace-only install'
$testInstall = Invoke-Adb -Arguments @('install', '--no-streaming', '-r', $TestApk)
Assert-InstallSucceeded -Result $testInstall -Label 'test replace-only install'

$registrations = Invoke-Adb -Arguments @('shell', 'pm', 'list', 'instrumentation')
$expectedRegistration = "instrumentation:$Runner (target=$TargetPackage)"
if ($registrations.Output.IndexOf($expectedRegistration, [System.StringComparison]::Ordinal) -lt 0) {
    throw "release instrumentation runner is not registered:`n$($registrations.Output)"
}

$fullMarkers = @(
    'TICKET170_INJECTED_FAILURES: PASS',
    'TICKET172_ACTIVITY_RECREATION: PASS',
    'RELEASE_CANDIDATE_INSTRUMENTATION: PASS',
    'INSTRUMENTATION_CODE: -1'
)
foreach ($pass in 1..2) {
    $fullOutput = Invoke-Instrumentation -EvidencePath (Join-Path $EvidenceDir ("full-instrumentation-$pass.txt"))
    Assert-InstrumentationOutput $fullOutput $fullMarkers "full instrumentation pass $pass"
}

$seedOutput = Invoke-Instrumentation `
    -ExtraArguments @('-e', 'ticket170_phase', 'seed') `
    -EvidencePath (Join-Path $EvidenceDir 'process-death-seed.txt')
$seedPrefix = 'TICKET170_PROCESS_DEATH_SEED: PASS token='
Assert-InstrumentationOutput $seedOutput @($seedPrefix, 'INSTRUMENTATION_CODE: -1') 'process-death seed'
$seedToken = Get-ProbeToken -Output $seedOutput -Prefix $seedPrefix

$null = Invoke-Adb -Arguments @('shell', 'am', 'force-stop', $TargetPackage)
$recoverOutput = Invoke-Instrumentation `
    -ExtraArguments @('-e', 'ticket170_phase', 'recover') `
    -EvidencePath (Join-Path $EvidenceDir 'process-death-recover.txt')
$recoverPrefix = 'TICKET170_PROCESS_DEATH_RECOVER: PASS token='
Assert-InstrumentationOutput $recoverOutput @($recoverPrefix, 'INSTRUMENTATION_CODE: -1') 'process-death recovery'
$recoverToken = Get-ProbeToken -Output $recoverOutput -Prefix $recoverPrefix
if ($recoverToken -ne $seedToken) {
    throw "process-death recovery token changed: seed=$seedToken recover=$recoverToken"
}

$installedTarget = Pull-InstalledBase -PackageName $TargetPackage -Destination (Join-Path $EvidenceDir 'installed-target-base.apk')
$installedTest = Pull-InstalledBase -PackageName $TestPackage -Destination (Join-Path $EvidenceDir 'installed-test-base.apk')
$candidateAppHash = (Get-FileHash -LiteralPath $AppApk -Algorithm SHA256).Hash
$candidateTestHash = (Get-FileHash -LiteralPath $TestApk -Algorithm SHA256).Hash
$installedAppHash = (Get-FileHash -LiteralPath $installedTarget -Algorithm SHA256).Hash
$installedTestHash = (Get-FileHash -LiteralPath $installedTest -Algorithm SHA256).Hash
if ($installedAppHash -ne $candidateAppHash) {
    throw 'installed target APK hash differs from candidate'
}
if ($installedTestHash -ne $candidateTestHash) {
    throw 'installed test APK hash differs from candidate'
}
if ((Get-SingleSignerSha256 -Apk $installedTarget -ApkSigner $apkSigner) -ne $candidateAppSigner) {
    throw 'installed target APK signer differs from candidate'
}
if ((Get-SingleSignerSha256 -Apk $installedTest -ApkSigner $apkSigner) -ne $candidateTestSigner) {
    throw 'installed test APK signer differs from candidate'
}

$model = (Invoke-Adb -Arguments @('shell', 'getprop', 'ro.product.model')).Output.Trim()
$api = (Invoke-Adb -Arguments @('shell', 'getprop', 'ro.build.version.sdk')).Output.Trim()
$summary = @(
    'RELEASE_DEVICE_GATE: PASS',
    "device_serial=$script:SelectedDevice",
    "device_model=$model",
    "device_api=$api",
    "signer_sha256=$candidateAppSigner",
    "app_sha256=$candidateAppHash",
    "test_sha256=$candidateTestHash",
    "process_death_token=$seedToken"
) -join "`n"
Save-Evidence -Path (Join-Path $EvidenceDir 'summary.txt') -Text $summary
Write-Host $summary
