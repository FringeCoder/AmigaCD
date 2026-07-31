<#
.SYNOPSIS
    Fetch a CI build and install it on the MiSTer.

.DESCRIPTION
    PowerShell counterpart to deploy-from-ci.sh, for driving deploys from a
    Windows shell without Git Bash. Same behaviour: resolve the newest
    successful `build` run unless one is named, download into a fresh temp
    directory, copy under a temp name and rename (atomic, and dodges ETXTBSY
    when the core is loaded), then echo the deployed md5.

    The host comes from .\host (gitignored) or $env:MISTER_HOST -- the same
    convention wsl-build.sh and build.sh use.

    Deploys the UNSTRIPPED MiSTer.elf by default. Identical code to the
    stripped MiSTer, ~200 KB larger, and the difference is that a crash under
    gdb gives a named backtrace instead of `?? ()`. Use -Stripped once
    bring-up is over.

.EXAMPLE
    .\scripts\deploy-from-ci.ps1
    Deploy the latest successful build.

.EXAMPLE
    .\scripts\deploy-from-ci.ps1 -RunId 30616420260 -Stripped
    Deploy a specific run, stripped binary.
#>
[CmdletBinding()]
param(
    [string] $RunId,
    [switch] $Stripped
)

$ErrorActionPreference = 'Stop'

if ($env:AMIGACD_REPO) { $RepoSlug = $env:AMIGACD_REPO } else { $RepoSlug = 'FringeCoder/AmigaCD' }
if ($env:AMIGACD_BINARY_NAME) { $BinaryName = $env:AMIGACD_BINARY_NAME } else { $BinaryName = 'MiSTer_AmigaCD' }
if ($Stripped) { $ArtifactFile = 'MiSTer' } else { $ArtifactFile = 'MiSTer.elf' }

$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw 'gh CLI not found on PATH.'
}

# NB: $Host is a reserved automatic variable in PowerShell, hence $MisterHost.
$MisterHost = $env:MISTER_HOST
if ([string]::IsNullOrWhiteSpace($MisterHost)) {
    $hostFile = Join-Path $RepoRoot 'host'
    if (Test-Path $hostFile) { $MisterHost = (Get-Content $hostFile -Raw).Trim() }
}
if ([string]::IsNullOrWhiteSpace($MisterHost)) {
    throw 'No MiSTer address. Put its IP in .\host or set $env:MISTER_HOST.'
}

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = gh run list --repo $RepoSlug --workflow build --status success --limit 1 --json databaseId --jq '.[0].databaseId'
    if ($LASTEXITCODE -ne 0) { throw 'gh run list failed.' }
    $RunId = "$RunId".Trim()
    if ([string]::IsNullOrWhiteSpace($RunId)) { throw "No successful build found on $RepoSlug." }
}

# A GUID temp directory rather than a fixed path: `gh run download` refuses to
# write into a directory that already exists, so a leftover from an earlier
# deploy would break the next one. Cleaned up in finally, on every exit path.
$Work = Join-Path ([System.IO.Path]::GetTempPath()) ('amigacd-' + [guid]::NewGuid().ToString('N'))

try {
    Write-Host "== run $RunId"
    gh run download $RunId --repo $RepoSlug --name MiSTer_AmigaCD --dir $Work
    if ($LASTEXITCODE -ne 0) { throw 'gh run download failed.' }

    $Artifact = Join-Path $Work $ArtifactFile
    if (-not (Test-Path $Artifact)) {
        throw "Artifact $ArtifactFile not present in the download."
    }
    Write-Host ("   {0}  {1:N0} bytes" -f $ArtifactFile, (Get-Item $Artifact).Length)

    Write-Host "== deploying $ArtifactFile to $MisterHost as /media/fat/$BinaryName"
    scp $Artifact "root@${MisterHost}:/media/fat/$BinaryName.new"
    if ($LASTEXITCODE -ne 0) { throw 'scp failed.' }

    # && here is fine: this string is parsed by the MiSTer's shell, not by
    # PowerShell, which has no pipeline chain operators in 5.1.
    $remote = "mv -f /media/fat/$BinaryName.new /media/fat/$BinaryName && " +
              "chmod +x /media/fat/$BinaryName && sync && " +
              "md5sum /media/fat/$BinaryName"
    ssh "root@$MisterHost" $remote
    if ($LASTEXITCODE -ne 0) { throw 'Remote install failed.' }

    Write-Host '== done. Reload the core to pick it up.'
}
finally {
    if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
}
