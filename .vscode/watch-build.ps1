$workspaceRoot = Resolve-Path "${PWD}"
Set-Location $workspaceRoot
Write-Host "[watch-build] Workspace root: $workspaceRoot"

$buildLock = $false
$timer = $null

function Invoke-Build {
    if ($buildLock) {
        return
    }
    $buildLock = $true
    Write-Host "[watch-build] Change detected. Running build..."
    $env:IDF_PATH = "D:\software\esp-idf\esp32\.espressif\v5.2.6\esp-idf"
    $buildCmd = "idf.py build"
    $process = Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile", "-Command", $buildCmd -Wait -NoNewWindow -PassThru
    if ($process.ExitCode -eq 0) {
        Write-Host "[watch-build] BUILD SUCCESS" -ForegroundColor Green
    } else {
        Write-Host "[watch-build] BUILD FAILED (exit code $($process.ExitCode))" -ForegroundColor Red
    }
    $buildLock = $false
}

$watcher = New-Object System.IO.FileSystemWatcher
$watcher.Path = $workspaceRoot
$watcher.IncludeSubdirectories = $true
$watcher.Filter = "*.*"
$watcher.NotifyFilter = [System.IO.NotifyFilters]'FileName, LastWrite, Size, CreationTime'
$watcher.EnableRaisingEvents = $true

$action = {
    $path = $Event.SourceEventArgs.FullPath
    if ($path -match '\\.(c|h|cpp|hpp|S|s|cmake|txt|json)$' -or $path -match 'CMakeLists\.txt$') {
        if ($timer) {
            $timer.Stop()
            $timer.Dispose()
        }
        $timer = New-Object System.Timers.Timer 1000
        $timer.AutoReset = $false
        $timer.add_Elapsed({ Invoke-Build })
        $timer.Start()
    }
}

Register-ObjectEvent $watcher Changed -Action $action | Out-Null
Register-ObjectEvent $watcher Created -Action $action | Out-Null
Register-ObjectEvent $watcher Renamed -Action $action | Out-Null
Write-Host "[watch-build] Watching source changes. Save a file to trigger build."

while ($true) {
    Start-Sleep -Seconds 1
}
