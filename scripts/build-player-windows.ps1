param(
    [Parameter(Mandatory = $true)][string]$FFmpegRoot,
    [string]$BuildDirectory = '',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$sdkRoot = Split-Path $PSScriptRoot -Parent
$ffmpegPrefix = (Resolve-Path -LiteralPath $FFmpegRoot).Path
if ((Split-Path $ffmpegPrefix -Leaf) -eq 'bin') { $ffmpegPrefix = Split-Path $ffmpegPrefix -Parent }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $sdkRoot 'out/player-windows-ffmpeg71' }
$dependencyDirectory = Join-Path $sdkRoot 'out/player-deps'
New-Item -ItemType Directory -Force -Path $dependencyDirectory | Out-Null

function Get-VerifiedArchive([string]$Url, [string]$Path, [string]$Sha256) {
    if (-not (Test-Path -LiteralPath $Path)) { Invoke-WebRequest $Url -OutFile $Path }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Sha256) {
        throw "Dependency checksum mismatch: $Path"
    }
}

$ffmpegInclude = Join-Path $ffmpegPrefix 'include'
if (-not (Test-Path -LiteralPath (Join-Path $ffmpegInclude 'libavformat/avformat.h'))) {
    # This particular 7.1 distribution contains DLLs/import libs but omits the
    # public headers. Use the exact upstream revision reported by its executable.
    $version = & (Join-Path $ffmpegPrefix 'bin/ffmpeg.exe') -version
    if ($LASTEXITCODE -ne 0 -or $version[0] -notmatch 'b08d796') {
        throw 'Missing FFmpeg headers. Supply the matching development package; automatic recovery supports b08d796 only.'
    }
    $revision = 'b08d7969c550a804a59511c7b83f2dd8cc0499b8'
    $archive = Join-Path $dependencyDirectory 'ffmpeg-b08d796.tar.gz'
    Get-VerifiedArchive "https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/$revision" $archive `
        '02FA6D9827DA3B6786E4DF821218CC036DB2B4481E7F48267C2DCDA695633AFC'
    $ffmpegInclude = Join-Path $dependencyDirectory "FFmpeg-$revision"
    if (-not (Test-Path -LiteralPath (Join-Path $ffmpegInclude 'libavformat/avformat.h'))) {
        & tar -xf $archive -C $dependencyDirectory
        if ($LASTEXITCODE -ne 0) { throw 'FFmpeg header extraction failed' }
    }
    # Public generated configuration for this MSVC little-endian x64 build.
    @'
#ifndef AVUTIL_AVCONFIG_H
#define AVUTIL_AVCONFIG_H
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
#endif
'@ | Set-Content -LiteralPath (Join-Path $ffmpegInclude 'libavutil/avconfig.h') -Encoding ascii
}
$sdlRoot = $ffmpegPrefix
if (-not (Test-Path -LiteralPath (Join-Path $sdlRoot 'include/SDL2/SDL.h'))) {
    $archive = Join-Path $dependencyDirectory 'SDL2-devel-2.30.9-VC.zip'
    Get-VerifiedArchive 'https://www.libsdl.org/release/SDL2-devel-2.30.9-VC.zip' $archive `
        '8C91D91E5BCB997D062EC2B553C53832EBF95654D4AA35E8C02A954D4CE752AE'
    $sdlRoot = Join-Path $dependencyDirectory 'SDL2-2.30.9'
    if (-not (Test-Path -LiteralPath (Join-Path $sdlRoot 'include/SDL.h'))) {
        Expand-Archive -LiteralPath $archive -DestinationPath $dependencyDirectory -Force
    }
}
# Clear discovery caches so changing FFmpeg versions cannot silently reuse old
# headers, import libraries or the fixture generator from another installation.
& cmake -S $sdkRoot -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 `
    -U 'LIVEKIT_FFMPEG_*' -U 'LIVEKIT_PLAYER_SDL_*' -U 'LIVEKIT_PLAYER_FFMPEG_EXECUTABLE' `
    -DLIVEKIT_BUILD_RTC=OFF -DLIVEKIT_BUILD_PLAYER=ON `
    "-DLIVEKIT_FFMPEG_ROOT=$ffmpegPrefix" "-DLIVEKIT_FFMPEG_INCLUDE=$ffmpegInclude" `
    "-DLIVEKIT_PLAYER_SDL_ROOT=$sdlRoot"
if ($LASTEXITCODE -ne 0) { throw 'Player configure failed' }
& cmake --build $BuildDirectory --config $Configuration --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Player build failed' }
& ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Player tests failed' }
