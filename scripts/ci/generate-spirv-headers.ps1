Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "../..")
$hlslRoot = Join-Path $repoRoot "UnleashedRecomp/gpu/shader/hlsl"
$artifactRoot = Join-Path $repoRoot ".artifacts/spirv-headers/UnleashedRecomp/gpu/shader/hlsl"
$toolRoot = Join-Path $repoRoot ".tools/dxc"

function Get-DxcPath {
    $existing = Get-Command dxc.exe -ErrorAction SilentlyContinue
    if ($existing) {
        return $existing.Source
    }

    New-Item -ItemType Directory -Force -Path $toolRoot | Out-Null

    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/microsoft/DirectXShaderCompiler/releases/latest"
    $asset = $release.assets |
        Where-Object { $_.name -match '^dxc_.*\.zip$' } |
        Select-Object -First 1

    if (-not $asset) {
        $assetNames = ($release.assets | ForEach-Object { $_.name }) -join ", "
        throw "Could not find a Windows DXC zip in the latest DirectXShaderCompiler release. Assets: $assetNames"
    }

    $zipPath = Join-Path $toolRoot $asset.name
    $extractPath = Join-Path $toolRoot "extract"

    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath
    if (Test-Path $extractPath) {
        Remove-Item -Recurse -Force $extractPath
    }

    Expand-Archive -Path $zipPath -DestinationPath $extractPath

    $downloaded = Get-ChildItem -Path $extractPath -Filter dxc.exe -Recurse | Select-Object -First 1
    if (-not $downloaded) {
        throw "Downloaded DXC archive did not contain dxc.exe"
    }

    return $downloaded.FullName
}

$shaderSpecs = @(
    @{ Name = "blend_color_alpha_ps"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "copy_vs"; Profile = "vs_6_0"; ExtraArgs = @("-fvk-invert-y", "-DUNLEASHED_RECOMP") },
    @{ Name = "copy_color_ps"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "copy_depth_ps"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "csd_filter_ps"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "csd_no_tex_vs"; Profile = "vs_6_0"; ExtraArgs = @("-fvk-invert-y", "-DUNLEASHED_RECOMP") },
    @{ Name = "csd_vs"; Profile = "vs_6_0"; ExtraArgs = @("-fvk-invert-y", "-DUNLEASHED_RECOMP") },
    @{ Name = "enhanced_motion_blur_ps"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "gaussian_blur_3x3"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "gaussian_blur_5x5"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "gaussian_blur_7x7"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "gaussian_blur_9x9"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "gamma_correction_ps"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "imgui_ps"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "imgui_vs"; Profile = "vs_6_0"; ExtraArgs = @("-fvk-invert-y", "-DUNLEASHED_RECOMP") },
    @{ Name = "movie_ps"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "movie_vs"; Profile = "vs_6_0"; ExtraArgs = @("-fvk-invert-y", "-DUNLEASHED_RECOMP") },
    @{ Name = "resolve_msaa_color_2x"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "resolve_msaa_color_4x"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "resolve_msaa_color_8x"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "resolve_msaa_depth_2x"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "resolve_msaa_depth_4x"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") },
    @{ Name = "resolve_msaa_depth_8x"; Profile = "ps_6_0"; ExtraArgs = @("-DUNLEASHED_RECOMP") }
)

$dxcPath = Get-DxcPath
New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

foreach ($shader in $shaderSpecs) {
    $inputPath = Join-Path $hlslRoot ($shader.Name + ".hlsl")
    $outputPath = $inputPath + ".spirv.h"
    $artifactPath = Join-Path $artifactRoot ($shader.Name + ".hlsl.spirv.h")

    $arguments = @(
        "-T", $shader.Profile,
        "-HV", "2021",
        "-all-resources-bound",
        "-spirv",
        "-fvk-use-dx-layout"
    ) + $shader.ExtraArgs + @(
        "-E", "shaderMain",
        "-Fh", $outputPath,
        $inputPath,
        "-Vn", ("g_" + $shader.Name + "_spirv")
    )

    Write-Host "Generating $outputPath"
    & $dxcPath @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "DXC failed while generating $outputPath"
    }

    Copy-Item -Force -Path $outputPath -Destination $artifactPath
}

Write-Host "Generated $($shaderSpecs.Count) SPIR-V headers"
