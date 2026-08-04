param(
    [Parameter(Mandatory = $true)]
    [string]$Binary
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$binaryPath = (Resolve-Path -LiteralPath $Binary).Path
$temporary = Join-Path ([IO.Path]::GetTempPath()) (
    'cldmux dispatch ' + [Guid]::NewGuid().ToString('N')
)
[void](New-Item -ItemType Directory -Path $temporary)

function Invoke-DryRun {
    param([Parameter(Mandatory = $true)][string]$Output)

    # Windows PowerShell 5.1 represents redirected native stderr as
    # non-terminating error records; explicit exit-code checks own this path.
    $ErrorActionPreference = 'Continue'
    $arguments = @(
        '--id=windows-0042'
        '--policy=gcp'
        '--image=image'
        "--input=$inputBundle"
        "--output=$Output"
        '--no-catalogue'
        '--'
        'cmd.exe'
        '/d'
        '/c'
        'echo hello'
    )
    $lines = @(& $binaryPath @arguments 2>&1 | ForEach-Object { "$_" })
    $exitCode = $LASTEXITCODE
    [PSCustomObject]@{ ExitCode = $exitCode; Text = $lines -join "`n" }
}

try {
    $inputBundle = Join-Path $temporary 'Case Input.tar.zst'
    $outputBundle = Join-Path $temporary 'Result Output.tar.zst'
    [IO.File]::WriteAllText($inputBundle, "immutable input`n")
    $before = [IO.File]::ReadAllBytes($inputBundle)

    $env:DISPATCH_INPUT_ROOT = 'cloud://dispatch-input'
    $env:DISPATCH_OUTPUT_ROOT = 'cloud://dispatch-output'
    $env:CLDMUX_GCP_PROJECT = 'test-project'
    $env:CLDMUX_GCP_REGION = 'europe-west4'

    $dryRun = Invoke-DryRun -Output $outputBundle
    if ($dryRun.ExitCode -ne 0 -or
        $dryRun.Text -notmatch '(?m)^program=dispatch$' -or
        $dryRun.Text -notmatch '(?m)^status=dry-run$' -or
        $dryRun.Text -notmatch '(?m)^approval=required$') {
        throw "Windows dry run failed:`n$($dryRun.Text)"
    }
    if (Test-Path -LiteralPath $outputBundle) {
        throw 'Windows dry run created its output bundle'
    }

    $caseCollision = Join-Path $temporary 'case input.tar.zst'
    $collision = Invoke-DryRun -Output $caseCollision
    if ($collision.ExitCode -ne 2 -or $collision.Text -ne
        'error=input, output, receipt, and pending receipt paths must be distinct') {
        throw "Windows case-only collision was not rejected:`n$($collision.Text)"
    }

    [IO.File]::WriteAllText($outputBundle, "existing output`n")
    $noClobber = Invoke-DryRun -Output $outputBundle
    if ($noClobber.ExitCode -ne 2 -or
        $noClobber.Text -ne 'error=output bundle already exists') {
        throw "Windows existing output was not rejected:`n$($noClobber.Text)"
    }
    if ([Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($outputBundle)) -ne
        "existing output`n") {
        throw 'Windows no-clobber check changed the existing output'
    }

    $after = [IO.File]::ReadAllBytes($inputBundle)
    if ([Convert]::ToBase64String($before) -ne [Convert]::ToBase64String($after)) {
        throw 'Windows dispatcher changed its input bundle'
    }
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
