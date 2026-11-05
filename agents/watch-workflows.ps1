# Live view of Claude Code workflow runs across all projects and sessions.
# Usage:
#   powershell -ExecutionPolicy Bypass -File agents\watch-workflows.ps1 [-ActiveMinutes 30] [-All] [-Once]
#   -ActiveMinutes  show runs whose files changed within this many minutes (default 30)
#   -All            show every run found, regardless of age
#   -Once           print one snapshot and exit instead of refreshing every 5 seconds
param(
    [int]$ActiveMinutes = 30,
    [switch]$All,
    [switch]$Once
)

$projectsRoot = Join-Path $env:USERPROFILE '.claude\projects'

function Get-WorkflowRuns {
    $cutoff = (Get-Date).AddMinutes(-$ActiveMinutes)
    Get-ChildItem -Path $projectsRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $project = $_
        Get-ChildItem -Path $project.FullName -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $session = $_
            $wfRoot = Join-Path $session.FullName 'subagents\workflows'
            if (-not (Test-Path $wfRoot)) { return }
            Get-ChildItem -Path $wfRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object {
                $runDir = $_
                $journal = Join-Path $runDir.FullName 'journal.jsonl'
                if (-not (Test-Path $journal)) { return }
                # Last activity = newest write to any file in the run directory.
                $lastWrite = (Get-ChildItem -Path $runDir.FullName -File |
                    Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
                if (-not $All -and $lastWrite -lt $cutoff) { return }
                # Workflow name comes from the persisted script file "<name>-<runId>.js".
                $runId = $runDir.Name
                $script = Get-ChildItem -Path (Join-Path $session.FullName 'workflows\scripts') `
                    -Filter "*-$runId.js" -ErrorAction SilentlyContinue | Select-Object -First 1
                $name = if ($script) { $script.BaseName -replace "-$([regex]::Escape($runId))$", '' } else { $runId }
                [pscustomobject]@{
                    Name      = $name
                    RunId     = $runId
                    Project   = $project.Name
                    RunDir    = $runDir.FullName
                    Journal   = $journal
                    Script    = if ($script) { $script.FullName } else { $null }
                    LastWrite = $lastWrite
                }
            }
        }
    } | Sort-Object LastWrite -Descending
}

# Reads the plan from the persisted workflow script. The journal has no plan, and
# agent labels are computed at runtime, so planned agents are a best-effort guess:
# each `label: `...${x.prop}...`` template is expanded with every `prop: '...'`
# value in the script. Templates that cannot be expanded are skipped, and the
# caller falls back to showing only the phases.
function Get-WorkflowPlan($scriptPath) {
    $plan = [pscustomobject]@{ Description = $null; Phases = @(); Agents = @() }
    if (-not $scriptPath -or -not (Test-Path $scriptPath)) { return $plan }
    $src = Get-Content -Path $scriptPath -Raw -ErrorAction SilentlyContinue
    if (-not $src) { return $plan }

    # meta block: description and phase titles (reliable for any workflow).
    $m = [regex]::Match($src, '(?s)export\s+const\s+meta\s*=\s*\{(.*?)\n\}')
    if ($m.Success) {
        $meta = $m.Groups[1].Value
        $d = [regex]::Match($meta, "description:\s*'((?:[^'\\]|\\.)*)'")
        if ($d.Success) { $plan.Description = $d.Groups[1].Value }
        $plan.Phases = @([regex]::Matches($meta, "title:\s*'((?:[^'\\]|\\.)*)'") | ForEach-Object { $_.Groups[1].Value })
    }

    # label templates, with the phase given next to them if present.
    $agents = New-Object System.Collections.ArrayList
    $labelRx = "label:\s*(?:``([^``]*)``|'([^']*)')(?:\s*,\s*phase:\s*'([^']*)')?"
    foreach ($lm in [regex]::Matches($src, $labelRx)) {
        $tpl = if ($lm.Groups[1].Success) { $lm.Groups[1].Value } else { $lm.Groups[2].Value }
        $phase = $lm.Groups[3].Value
        $holes = @([regex]::Matches($tpl, '\$\{([^}]*)\}'))
        if ($holes.Count -eq 0) {
            [void]$agents.Add([pscustomobject]@{ Label = $tpl; Phase = $phase }); continue
        }
        # Only one placeholder of the form ${ident.prop} can be expanded.
        if ($holes.Count -ne 1) { continue }
        $pm = [regex]::Match($holes[0].Groups[1].Value, '^\s*\w+\.(\w+)\s*$')
        if (-not $pm.Success) { continue }
        $prop = [regex]::Escape($pm.Groups[1].Value)
        $values = @([regex]::Matches($src, "\b${prop}:\s*'([^']*)'") | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
        foreach ($v in $values) {
            [void]$agents.Add([pscustomobject]@{ Label = $tpl.Replace($holes[0].Value, $v); Phase = $phase })
        }
    }
    $plan.Agents = @($agents)
    return $plan
}

function Show-Run($run) {
    $plan = Get-WorkflowPlan $run.Script
    $agents = [ordered]@{}
    foreach ($line in Get-Content -Path $run.Journal -ErrorAction SilentlyContinue) {
        try { $e = $line | ConvertFrom-Json } catch { continue }
        if (-not $e.agentId) { continue }
        if ($e.type -eq 'started') {
            $agents[$e.agentId] = [pscustomobject]@{ Label = $e.label; Phase = $e.phase; Done = $false }
        } elseif ($e.type -eq 'result' -and $agents.Contains($e.agentId)) {
            $agents[$e.agentId].Done = $true
        }
    }
    $doneCount = @($agents.Values | Where-Object Done).Count
    $startedLabels = @($agents.Values | ForEach-Object { $_.Label })
    $planned = @($plan.Agents | Where-Object { $startedLabels -notcontains $_.Label })
    $total = $agents.Count + $planned.Count
    $age = [int]((Get-Date) - $run.LastWrite).TotalMinutes
    Write-Host ("{0}  [{1}]  {2}/{3} agents done  (last activity {4} min ago)" -f `
        $run.Name, $run.RunId, $doneCount, $total, $age) -ForegroundColor Cyan
    Write-Host ("  project: {0}" -f $run.Project) -ForegroundColor DarkGray
    if ($plan.Description) { Write-Host ("  {0}" -f $plan.Description) -ForegroundColor DarkGray }
    if ($plan.Phases.Count -gt 0) {
        Write-Host ("  phases: {0}" -f ($plan.Phases -join ' > ')) -ForegroundColor DarkGray
        if ($plan.Agents.Count -eq 0) {
            Write-Host '  planned agents: unknown (labels computed at runtime)' -ForegroundColor DarkGray
        }
    }
    foreach ($id in $agents.Keys) {
        $a = $agents[$id]
        if ($a.Done) {
            $state = 'DONE'; $color = 'Green'
        } else {
            # A running agent appends to its transcript; long silence suggests it died.
            $transcript = Join-Path $run.RunDir "agent-$id.jsonl"
            $idle = if (Test-Path $transcript) { [int]((Get-Date) - (Get-Item $transcript).LastWriteTime).TotalMinutes } else { 0 }
            if ($idle -ge 10) { $state = "stalled? (idle $idle min)"; $color = 'Red' }
            else { $state = 'running'; $color = 'Yellow' }
        }
        Write-Host ("  {0,-8} {1,-28} {2}" -f $a.Phase, $a.Label, $state) -ForegroundColor $color
    }
    foreach ($p in $planned) {
        Write-Host ("  {0,-8} {1,-28} {2}" -f $p.Phase, $p.Label, 'planned') -ForegroundColor DarkGray
    }
    Write-Host ''
}

while ($true) {
    $runs = @(Get-WorkflowRuns)
    if (-not $Once) { Clear-Host }
    Write-Host ("Claude workflows  {0}  ({1})`n" -f (Get-Date -Format 'HH:mm:ss'),
        $(if ($All) { 'all runs' } else { "active in last $ActiveMinutes min" }))
    if ($runs.Count -eq 0) { Write-Host '  No workflow runs found.' -ForegroundColor DarkGray }
    foreach ($run in $runs) { Show-Run $run }
    if ($Once) { break }
    Start-Sleep -Seconds 1
}
