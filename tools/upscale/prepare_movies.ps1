# Converts every Lightning Returns movie to MP4, then upscales them with Real-ESRGAN (resumable; below normal priority).
# prepare_movies.ps1 -Game <game dir> [-Height 2160] [-Model RealESRGAN_x4plus.pth]
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [int]$Height = 2160,
    [string]$Model = 'RealESRGAN_x4plus.pth',
    [string]$Tools = "$env:LOCALAPPDATA\ff13-launcher\tools"
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
$lr = Join-Path $root 'build\win\lr.exe'
$movies = Join-Path $Game 'weiss_data\movie'
$status = Join-Path $movies 'xrc\upscale_status.txt'
$env:XRC_MOVIE_CONVERTER = Join-Path $Tools 'bink2mp4.exe'
$env:XRC_MOVIE_FFMPEG = Join-Path $Tools 'ffmpeg.exe'
Remove-Item Env:XRC_MOVIE_NO_CONVERT -ErrorAction SilentlyContinue
Remove-Item Env:XRC_MOVIE_HD_HEIGHT -ErrorAction SilentlyContinue
"converting movies" | Set-Content $status
$p = Start-Process $lr "--convert-movies `"$movies`"" -WorkingDirectory $Game -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $movies 'xrc\convert_all.log')
$p.PriorityClass = 'BelowNormal'
$p.WaitForExit()
& (Join-Path $here '.venv\Scripts\python.exe') -I (Join-Path $here 'upscale_movies.py') $Game --height $Height --model (Join-Path $here $Model) --ffmpeg $env:XRC_MOVIE_FFMPEG --status $status
