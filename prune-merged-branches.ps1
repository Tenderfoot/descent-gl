#Requires -Version 5.1
<#
    Prunes merged branches on Tenderfoot/descent-gl.

    Deletes 92 remote branches verified as merged into main.
    KEEPS: main, eloquinlines, mapchanges, claude/focused-gauss-ovm5i9
           (the last three hold content that exists nowhere in main).

    Verified 2026-09-20 against full history. NOTE: the analysis clone was
    shallow; results were re-verified after `git fetch --unshallow`.

    Run from anywhere inside your descent-gl clone.
#>

$ErrorActionPreference = 'Stop'

$protected = @('main', 'eloquinlines', 'mapchanges', 'claude/focused-gauss-ovm5i9')

$branches = @(
    'claude/admiring-galileo-t8j7v0',
    'claude/admiring-planck-x738ol',
    'claude/automap-wheel-zoom',
    'claude/binary-combat-music',
    'claude/bold-knuth-s8ap1h',
    'claude/convex-hull-proxies',
    'claude/cool-brahmagupta-dtunb3',
    'claude/dreamy-curie-pmox2m',
    'claude/eager-lovelace-q7m2vt',
    'claude/editor-generator',
    'claude/editor-gizmo-light-leak',
    'claude/editor-moving-lights',
    'claude/elegant-mccarthy-0hngl4',
    'claude/elegant-mccarthy-0hngl4-lights',
    'claude/elegant-mccarthy-0hngl4-shadow-precision',
    'claude/eloquent-brown-iuq714',
    'claude/fervent-mayer-6gp6zs',
    'claude/fervent-mayer-6gp6zs-chase-default',
    'claude/fervent-mayer-6gp6zs-editor-hatch',
    'claude/fervent-mayer-6gp6zs-hatch',
    'claude/fervent-mayer-6gp6zs-timers',
    'claude/friendly-turing-shaz41',
    'claude/generator-rename',
    'claude/gifted-goodall-50a6oy',
    'claude/great-franklin-4hbnop',
    'claude/great-hypatia-3rpdlj',
    'claude/hud-music-marquee',
    'claude/hud-strip-scale',
    'claude/hud-warnings',
    'claude/hud-weapon-slot-contract',
    'claude/hud-weapon-slot-fill',
    'claude/hud-weapon-strip',
    'claude/keen-lovelace-q85aiy',
    'claude/keen-lovelace-q85aiy-logo',
    'claude/linux-build',
    'claude/m1-6dof-flight',
    'claude/m2-segments-lights',
    'claude/m3-shadow-volumes',
    'claude/m4-collision',
    'claude/m5-weapons',
    'claude/m6-robots',
    'claude/m7-docs-hud-gameflow',
    'claude/m7-game-loop',
    'claude/m8-assets',
    'claude/m8-automap',
    'claude/m8-feel',
    'claude/m8-perf',
    'claude/magical-goodall-yhm8f1',
    'claude/modest-cerf-6vxaug',
    'claude/music-playlist-next-track',
    'claude/nice-pascal-hv9sxu',
    'claude/peaceful-newton-otxi64',
    'claude/pickup-sprites',
    'claude/prop-lights-detection',
    'claude/relaxed-noether-g5esyb',
    'claude/serene-thompson-rrqfek',
    'claude/tender-hypatia-98agym',
    'claude/vibrant-volta-c4us78',
    'claude/wizardly-ptolemy-gv0n9k',
    'claude/wonderful-ptolemy-hb8lnv',
    'claude/wonderful-ptolemy-hb8lnv-e3',
    'claude/wonderful-ptolemy-hb8lnv-e4',
    'claude/wonderful-ptolemy-hb8lnv-e5',
    'claude/youthful-einstein-jwezt0',
    'claude/youthful-einstein-jwezt0-menu',
    'claude/youthful-einstein-jwezt0-switch',
    'codex/adaptive-music',
    'codex/camera-fov-settings',
    'codex/fix-combat-hit-segments',
    'codex/fix-editor-entity-palette',
    'codex/fix-gpu-light-timing',
    'codex/fix-projectiles-through-opening-doors',
    'codex/fix-settings-1440p',
    'codex/fix-shadow-volume-closure',
    'codex/fix-vsync-mouse-input',
    'codex/lua-multiclip',
    'codex/lua-support',
    'codex/player-shell-phase-1',
    'codex/player-shell-phase-2',
    'codex/player-shell-phase-3',
    'codex/player-shell-phase-4',
    'codex/player-shell-phase-5',
    'codex/split-main-responsibilities',
    'codex/target-hud',
    'codex/video-comm',
    'comms_update',
    'editor_update_join',
    'keyweaponbehaviour',
    'marketing',
    'mineentrancechanges',
    'musicandmarketing',
    'new_images'
)

# Safety guard: refuse to run if a protected branch ever enters the list.
$collision = $branches | Where-Object { $protected -contains $_ }
if ($collision) {
    throw "Refusing to run: protected branch(es) in delete list: $($collision -join ', ')"
}

Write-Host "Deleting $($branches.Count) merged branches..." -ForegroundColor Cyan

# PowerShell expands an array into separate args for native commands.
$refspecs = $branches | ForEach-Object { ":refs/heads/$_" }
git push origin @refspecs
if ($LASTEXITCODE -ne 0) { throw "git push failed with exit code $LASTEXITCODE" }

git fetch --prune origin | Out-Null

Write-Host "`nRemaining remote branches:" -ForegroundColor Green
git ls-remote --heads origin | ForEach-Object { ($_ -split 'refs/heads/')[1] }
