#
#  installer-kit.ps1
#  tests-manual/make-link/windows-box
#
#  Make v1 on Windows: the installer proof kit (security's check 5, release's run list steps 9-10). Shows that the
#  installer's two firewall rules close the box's one gap (the laptop's own LAN address), that Make's own profile steps
#  leave the rules alone, and that the gap is back once the rules are gone. Run as the normal user, never elevated
#  (make-box-test.exe refuses to run elevated). Two phases, around the installer's own steps:
#    after the install:    powershell -ExecutionPolicy Bypass -File installer-kit.ps1 -Phase installed -Out <folder>
#    after the uninstall:  powershell -ExecutionPolicy Bypass -File installer-kit.ps1 -Phase uninstalled -Out <folder>
#  Same -Out both times. The installed phase keeps a copy of the installer's rule check for the uninstalled phase,
#  since the uninstall removes the original. Each step prints PASS, FAIL or SKIP. The last line is "INSTALLER KIT
#  <phase>: PASS", "... FAIL (<n>)" or "... INCOMPLETE (<n> skipped)"; the exit status is 0 only on PASS.
#  Make's real profile is deleted only when the kit made it: if Make made Darkride.MakeBlender on this machine before,
#  the kit keeps it (and its folder), skips the steps that need a fresh one, and says so (security, 11-07).
#  installed:   1. the rules check (darkride-firewall-rules.exe check) is 0;
#               2. Make's profile made as Make makes it, its SID the rules' SID; the rules check still 0;
#               3. the box in Make's profile: the unboxed control reaches Windows' port 135 on the LAN address and gets
#                  an answer at a closed port there; the box reaches NEITHER; the rest of the probe passes;
#               4. Make's profile deleted as Make deletes it; the rules check still 0 (both rules unchanged).
#  uninstalled: 5. the rules check is 7 (no rule of ours left);
#               6. the box in Make's profile reaches the LAN address again (so step 3's refusal was the rules').
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#
param(
    [Parameter(Mandatory = $true)][ValidateSet('installed', 'uninstalled')][string]$Phase,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Blender = 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe',
    [string]$BoxTest = (Join-Path $PSScriptRoot 'build\make-box-test.exe'),
    [string]$Helper = (Join-Path $env:ProgramFiles 'Darkride\setup\darkride-firewall-rules.exe'),
    [string]$Mac = ''  # optional: <address>:<port> of a listener on another machine, as in run 6
)
$ErrorActionPreference = 'Continue'  # native programs' stderr must not stop the kit; every step is checked itself
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $Out "kit-$Phase.txt"
$script:fails = 0
$script:skips = 0

function Say([string]$line) {
    $line | Tee-Object -FilePath $log -Append
}

function Skip([string]$name, [string]$why) {
    $script:skips++
    Say "SKIP  $name  ($why)"
}

# Make's profile made as Make makes it; make-box-test's exit: 0 made, 5 made but its SID isn't the rules', 1 there
# already (not the kit's: kept), 3 not made
function MakeProfile([string]$record) {
    & $BoxTest --profile-create *>&1 | Out-File -FilePath (Join-Path $Out $record)
    return $LASTEXITCODE
}

function Expect([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { $mark = 'PASS' } else { $mark = 'FAIL'; $script:fails++ }
    Say "$mark  $name  ($detail)"
}

function RulesCheck([string]$exe, [string]$label) {
    if (-not (Test-Path $exe)) { return -1 }
    "--- $label" | Out-File -FilePath (Join-Path $Out "rules-check-$Phase.txt") -Append
    & $exe check *>&1 | Out-File -FilePath (Join-Path $Out "rules-check-$Phase.txt") -Append
    return $LASTEXITCODE
}

function CheckNamed($results, [string]$like) {
    if ($null -eq $results) { return $null }
    return @($results.results | Where-Object { $_.check -like $like })[0]
}

# always --keep-profile: the kit alone decides whether Make's profile is deleted
function BoxRun([string]$folder) {
    $arguments = @($Blender, $folder, '--profile', 'Darkride.MakeBlender', '--system-port', '--keep-profile')
    if ($Mac) { $arguments += @('--mac', $Mac) }
    & $BoxTest @arguments *>&1 | Out-File -FilePath "$folder-console.txt"
    $code = $LASTEXITCODE
    $file = Join-Path $folder 'results.json'
    if (-not (Test-Path $file)) { return @{ code = $code; results = $null } }
    return @{ code = $code; results = (Get-Content $file -Raw | ConvertFrom-Json) }
}

Say "installer kit, phase $Phase, $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
$helperCopy = Join-Path $Out 'rules-check-copy\darkride-firewall-rules.exe'

if ($Phase -eq 'installed') {
    New-Item -ItemType Directory -Force -Path (Split-Path $helperCopy) | Out-Null
    Copy-Item -Path $Helper -Destination $helperCopy -Force
    $c = RulesCheck $Helper 'step 1: after the install'
    Expect '1. the installer''s rules check after the install' ($c -eq 0) "exit $c"

    $p = MakeProfile 'profile-create.txt'
    $madeByKit = ($p -eq 0) -or ($p -eq 5)
    if ($p -eq 1) {
        Skip '2a. Make''s profile made, its SID the rules'' SID' 'Make''s profile was already there: kept, not made by the kit'
        Skip '2b. the rules check after the profile is made' 'no fresh profile to show it'
    } else {
        Expect '2a. Make''s profile made, its SID the rules'' SID' ($p -eq 0) "exit $p"
        $c = RulesCheck $Helper 'step 2: after Make''s profile is made'
        Expect '2b. the rules check after the profile is made' ($c -eq 0) "exit $c"
    }

    $run = BoxRun (Join-Path $Out 'box-installed')
    $r = $run.results
    Expect '3a. the box test ran in Make''s profile' (($null -ne $r) -and ($r.form -ne 'none started') -and ($r.profile -eq 'Darkride.MakeBlender')) "exit $($run.code)"
    $control135 = CheckNamed $r.control '*port 135*'
    $controlClosed = CheckNamed $r.control '*closed port*'
    $box135 = CheckNamed $r.probe '*port 135*'
    $boxClosed = CheckNamed $r.probe '*closed port*'
    Expect '3b. unboxed control: port 135 on the LAN address connects' (($null -ne $control135) -and ($control135.pass -eq $true)) "$($control135.detail)"
    Expect '3c. unboxed control: the closed port on the LAN address answers' (($null -ne $controlClosed) -and ($controlClosed.pass -eq $true)) "$($controlClosed.detail)"
    Expect '3d. the box does NOT reach port 135 on the LAN address' (($null -ne $box135) -and ($box135.reached -eq $false)) "$($box135.detail)"
    Expect '3e. the box does NOT reach the closed port on the LAN address' (($null -ne $boxClosed) -and ($boxClosed.reached -eq $false)) "$($boxClosed.detail)"
    if ($null -ne $r) {
        # the DNS checks are reported, not scored: run 6 shows what the box does there, security rules on it
        $failed = @($r.probe.results | Where-Object { ($_.pass -eq $false) -and ($_.check -notlike '*(DNS)*') })
        Expect '3f. every other probe check passes' ($failed.Count -eq 0) "$($failed.Count) failed: $(($failed | ForEach-Object { $_.check }) -join '; ')"
        foreach ($dns in @($r.probe.results | Where-Object { $_.check -like '*(DNS)*' })) {
            Say "INFO  3h. in the box, $($dns.check): reached $($dns.reached) ($($dns.detail))"
        }
        Expect '3g. killing the parent ends Blender' ($r.kill_the_parent_ends_blender -eq $true) "$($r.kill_the_parent_ends_blender)"
    }

    if ($madeByKit) {
        & $BoxTest --profile-delete *>&1 | Out-File -FilePath (Join-Path $Out 'profile-delete.txt')
        $d = $LASTEXITCODE
        Expect '4a. Make''s profile deleted' ($d -eq 0) "exit $d"
        $c = RulesCheck $Helper 'step 4: after Make''s profile is deleted'
        Expect '4b. the rules check after the profile is deleted (both rules unchanged)' ($c -eq 0) "exit $c"
    } else {
        Skip '4a. Make''s profile deleted' 'the kit didn''t make it: kept'
        Skip '4b. the rules check after the profile is deleted' 'no delete to show it'
    }
} else {
    $c = RulesCheck $helperCopy 'step 5: after the uninstall'
    Expect '5. the rules check after the uninstall finds no rule of ours' ($c -eq 7) "exit $c"

    $p = MakeProfile 'profile-create-uninstalled.txt'
    $run = BoxRun (Join-Path $Out 'box-uninstalled')
    if (($p -eq 0) -or ($p -eq 5)) {
        & $BoxTest --profile-delete *>&1 | Out-File -FilePath (Join-Path $Out 'profile-delete-uninstalled.txt')
    } elseif ($p -eq 1) {
        Say "INFO  Make's profile was already there: kept"
    }
    $r = $run.results
    $control135 = CheckNamed $r.control '*port 135*'
    $box135 = CheckNamed $r.probe '*port 135*'
    $boxClosed = CheckNamed $r.probe '*closed port*'
    Expect '6a. unboxed control: port 135 on the LAN address connects' (($null -ne $control135) -and ($control135.pass -eq $true)) "$($control135.detail)"
    Expect '6b. the box reaches port 135 on the LAN address again' (($null -ne $box135) -and ($box135.reached -eq $true)) "$($box135.detail)"
    Expect '6c. the box reaches the closed port on the LAN address again' (($null -ne $boxClosed) -and ($boxClosed.reached -eq $true)) "$($boxClosed.detail)"
}

if ($script:fails -gt 0) { Say "INSTALLER KIT ${Phase}: FAIL ($script:fails)"; exit 1 }
if ($script:skips -gt 0) { Say "INSTALLER KIT ${Phase}: INCOMPLETE ($script:skips skipped)"; exit 2 }
Say "INSTALLER KIT ${Phase}: PASS"
exit 0
