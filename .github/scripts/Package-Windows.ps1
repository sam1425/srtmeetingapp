[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [string] $CodesignPath = '',
    [string] $CodesignPassword = ''
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse
    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path "${ProjectRoot}/buildspec.json" -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductDisplayName = $BuildSpec.displayName
    $ProductVersion = $BuildSpec.version
    $ProductAuthor = $BuildSpec.author
    $ProductWebsite = $BuildSpec.website

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"
    $StageDir = Resolve-Path -Path "${ProjectRoot}/release/${Configuration}"

    # Clean old artifacts
    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.exe",
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        )
    }
    Remove-Item @RemoveArgs

    # --- Step 1: Create .zip archive (fallback for manual install) ---
    Log-Group "Archiving ${ProductName} (zip)..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${StageDir}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    # --- Step 2: Build Inno Setup installer ---
    $IssTemplate = "${ProjectRoot}/cmake/windows/installer.iss.in"
    $IssFile = "${ProjectRoot}/release/${OutputName}.iss"
    $InstallerExe = "${ProjectRoot}/release/${OutputName}.exe"

    if (Test-Path $IssTemplate) {
        Log-Group "Building Inno Setup installer..."

        # Find ISCC.exe (pre-installed on GitHub Actions windows-2022)
        $IsccPaths = @(
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
            "${env:ProgramFiles(x86)}\Inno Setup\ISCC.exe",
            "${env:ProgramFiles}\Inno Setup\ISCC.exe"
        )
        $Iscc = $null
        foreach ($p in $IsccPaths) {
            if (Test-Path $p) { $Iscc = $p; break }
        }

        if (-not $Iscc) {
            Write-Warning "Inno Setup (ISCC.exe) not found. Installer will not be built."
            return
        }

        # Read template and substitute placeholders
        $IssContent = Get-Content -Path $IssTemplate -Raw
        $IssContent = $IssContent -replace '__PLUGIN_DISPLAY_NAME__', $ProductDisplayName
        $IssContent = $IssContent -replace '__PLUGIN_NAME__', $ProductName
        $IssContent = $IssContent -replace '__PLUGIN_VERSION__', $ProductVersion
        $IssContent = $IssContent -replace '__PLUGIN_AUTHOR__', $ProductAuthor
        $IssContent = $IssContent -replace '__PLUGIN_WEBSITE__', $ProductWebsite
        $IssContent = $IssContent -replace '__TARGET_ARCH__', $Target

        Set-Content -Path $IssFile -Value $IssContent -NoNewline

        # Compile installer
        $BuildDir = $StageDir  # The install prefix already has OBS plugin layout
        $IsccArgs = @(
            "/dBUILD_DIR=$BuildDir",
            "/dOUTPUT_DIR=${ProjectRoot}/release",
            $IssFile
        )
        Write-Debug "Running: & '$Iscc' $($IsccArgs -join ' ')"
        & $Iscc @IsccArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Inno Setup compilation failed with exit code $LASTEXITCODE"
        }

        # Sign the installer if certificate is provided
        if ($CodesignPath -and (Test-Path $CodesignPath)) {
            Log-Group "Signing installer (Authenticode)..."
            $SignTool = Get-Command 'signtool.exe' -ErrorAction SilentlyContinue
            if (-not $SignTool) {
                # signtool is typically in the Windows SDK
                $SignToolPaths = @(
                    "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe",
                    "${env:ProgramFiles}\Windows Kits\10\bin\*\x64\signtool.exe"
                )
                $SignTool = Get-ChildItem $SignToolPaths | Sort-Object -Property FullName -Descending | Select-Object -First 1
            }
            if ($SignTool) {
                $SignArgs = @(
                    'sign', '/fd', 'SHA256',
                    '/f', $CodesignPath
                )
                if ($CodesignPassword) {
                    $SignArgs += '/p'
                    $SignArgs += $CodesignPassword
                }
                $SignArgs += '/tr', 'http://timestamp.digicert.com'
                $SignArgs += '/td', 'SHA256'
                $SignArgs += $InstallerExe

                Write-Debug "Running: '$($SignTool.Path)' $($SignArgs -join ' ')"
                & $SignTool @SignArgs
                if ($LASTEXITCODE -eq 0) {
                    Write-Debug "Installer signed successfully."
                } else {
                    Write-Warning "Installer signing failed with exit code $LASTEXITCODE (non-fatal)."
                }
            } else {
                Write-Warning "signtool.exe not found — cannot sign installer (non-fatal)."
            }
            Log-Group
        }

        # Clean up temporary .iss file
        Remove-Item -Path $IssFile -Force -ErrorAction SilentlyContinue

        Log-Group
    } else {
        Write-Warning "Inno Setup template not found at ${IssTemplate}. Only zip will be created."
    }
}

Package
