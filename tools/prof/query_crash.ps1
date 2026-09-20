param(
    [ValidateRange(1,1000)][int]$Count = 8,
    [ValidateRange(1,365)][int]$Days = 7
)
$ErrorActionPreference = 'Stop'
# Filter the application in event data before limiting the number of matches.
$age = [long]$Days * 86400000
$filter = "*[System[(EventID=1000) and TimeCreated[timediff(@SystemTime) <= $age]] and *[EventData[Data[@Name='AppName']='eden.exe']]"
Get-WinEvent -LogName Application -FilterXPath $filter -MaxEvents $Count |
    ForEach-Object {
        Write-Output ("=== " + $_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss'))
        Write-Output $_.Message
    }
