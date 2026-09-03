# Spektrafilm Android — fetch the digest-pinned, upstream AOSP CTS decoder corpus.
# This is a preparation step only; connected qualification runs remain offline.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
$root = [System.IO.Path]::GetPathRoot($resolvedOutput)
if ($resolvedOutput.TrimEnd('\', '/') -eq $root.TrimEnd('\', '/')) {
    throw 'OutputDirectory must not be a filesystem root.'
}
[System.IO.Directory]::CreateDirectory($resolvedOutput) | Out-Null

$mainCommit = '1daba777fa1cc472226da4104041849ccbc65b80'
$android11Commit = '41ac7864bded41f9c042bd9e3cf9a2c083f23da9'
$items = @(
    @{
        Name = 'translucent-green-p3.png'
        Sha256 = '9f1bd663564634bff9d7f3c25a9495ac71c8565a6a9407a64acfae2bc33e1c57'
        Url = "https://android.googlesource.com/platform/cts/+/$mainCommit/tests/tests/graphics/assets/translucent-green-p3.png?format=TEXT"
    },
    @{
        Name = 'blue-16bit-srgb.png'
        Sha256 = 'aa39b12b96bba7084648902af956f6563f361d8631400396344807ce919cb6db'
        Url = "https://android.googlesource.com/platform/cts/+/$mainCommit/tests/tests/graphics/assets/blue-16bit-srgb.png?format=TEXT"
    },
    @{
        Name = 'red-hlg-profile.png'
        Sha256 = '9cf5df965aefb69ac6dc9845055c8a84309879dc1f451074cb632159cbb4a193'
        Url = "https://android.googlesource.com/platform/cts/+/$mainCommit/tests/tests/graphics/assets/red-hlg-profile.png?format=TEXT"
    },
    @{
        Name = 'red-pq-profile.png'
        Sha256 = 'a2b2a147067b0e019ed7768abc424dc7755694fe920021517d3be8257338cb6b'
        Url = "https://android.googlesource.com/platform/cts/+/$mainCommit/tests/tests/graphics/assets/red-pq-profile.png?format=TEXT"
    },
    @{
        Name = 'animated.gif'
        Sha256 = 'eec5e745032b9775d67f040d9ab95ae3dc296100ce0c5d6bf95667bf2d27d2a6'
        Url = "https://android.googlesource.com/platform/cts/+/$mainCommit/tests/tests/graphics/res/drawable/animated.gif?format=TEXT"
    },
    @{
        Name = 'heifwriter_input.heic'
        Sha256 = '62dfb44160403ca8355a874cecc91cdbce57e98dd597fa36a2af55ef54c017ac'
        Url = "https://android.googlesource.com/platform/cts/+/$android11Commit/tests/tests/media/res/raw/heifwriter_input.heic?format=TEXT"
    },
    @{
        Name = 'sample_1mp.dng'
        Sha256 = '271aa1db6369f271e160acaf3029c8e86b8a86d2e9a44d1cc731f50575767ac0'
        Url = "https://android.googlesource.com/platform/cts/+/$mainCommit/tests/tests/graphics/res/raw/sample_1mp.dng?format=TEXT"
    },
    @{
        Name = 'bug_156261521.dng'
        Sha256 = '8b0237910cc4ff180ad96fb1af42ef4a5b1edd92f9fa2cb274638a97b20db544'
        Url = "https://android.googlesource.com/platform/cts/+/$mainCommit/tests/tests/security/res/raw/bug_156261521.dng?format=TEXT"
    }
)

$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    foreach ($item in $items) {
        $destination = Join-Path $resolvedOutput $item.Name
        if ([System.IO.File]::Exists($destination)) {
            $existing = [System.IO.File]::ReadAllBytes($destination)
            $existingDigest = [System.BitConverter]::ToString(
                $sha256.ComputeHash($existing)
            ).Replace('-', '').ToLowerInvariant()
            if ($existingDigest -ne $item.Sha256) {
                throw "Existing $destination has digest $existingDigest; refusing to overwrite it."
            }
            Write-Host "verified $($item.Name) $existingDigest"
            continue
        }

        $encoded = (Invoke-WebRequest -UseBasicParsing -Uri $item.Url).Content.Trim()
        $bytes = [System.Convert]::FromBase64String($encoded)
        $digest = [System.BitConverter]::ToString(
            $sha256.ComputeHash($bytes)
        ).Replace('-', '').ToLowerInvariant()
        if ($digest -ne $item.Sha256) {
            throw "Downloaded $($item.Name) has digest $digest, expected $($item.Sha256)."
        }
        $temporary = Join-Path $resolvedOutput ('.' + $item.Name + '.' + [System.Guid]::NewGuid() + '.partial')
        try {
            [System.IO.File]::WriteAllBytes($temporary, $bytes)
            [System.IO.File]::Move($temporary, $destination)
        } finally {
            if ([System.IO.File]::Exists($temporary)) {
                [System.IO.File]::Delete($temporary)
            }
        }
        Write-Host "fetched $($item.Name) $digest"
    }
} finally {
    $sha256.Dispose()
}

Write-Host "AIMAGE_CORPUS: VERIFIED $resolvedOutput"
