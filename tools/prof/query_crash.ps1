Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000} -MaxEvents 8 -ErrorAction SilentlyContinue |
  Where-Object { $_.Message -match 'eden' } |
  ForEach-Object {
    $lines = ($_.Message -split "`r?`n") | Select-Object -First 12
    Write-Output ("=== " + $_.TimeCreated.ToString('HH:mm:ss'))
    $lines | Write-Output
  }
