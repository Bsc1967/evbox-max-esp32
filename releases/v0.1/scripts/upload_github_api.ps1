$ErrorActionPreference = "Stop"

$Gh = "C:\Program Files\GitHub CLI\gh.exe"
$Repo = "Bsc1967/evbox-max-esp32"
$Message = $args[0]
$Files = $args[1..($args.Length - 1)]

function Invoke-GhJson {
  param(
    [string] $Method,
    [string] $Endpoint,
    [object] $Body = $null
  )

  if ($null -eq $Body) {
    $Raw = & $Gh api $Endpoint
  } else {
    $Temp = New-TemporaryFile
    try {
      $Json = $Body | ConvertTo-Json -Depth 30
      [System.IO.File]::WriteAllText($Temp.FullName, $Json, [System.Text.UTF8Encoding]::new($false))
      $Raw = & $Gh api --method $Method $Endpoint --input $Temp
    } finally {
      Remove-Item -LiteralPath $Temp -Force -ErrorAction SilentlyContinue
    }
  }

  if ($LASTEXITCODE -ne 0) {
    throw "gh api failed: $Endpoint"
  }

  return $Raw | ConvertFrom-Json
}

if ($args.Length -lt 2) {
  throw "Usage: upload_github_api.ps1 <commit-message> <file> [file...]"
}

$Ref = Invoke-GhJson "GET" "repos/$Repo/git/ref/heads/main"
$BaseCommitSha = $Ref.object.sha
$BaseCommit = Invoke-GhJson "GET" "repos/$Repo/git/commits/$BaseCommitSha"
$BaseTreeSha = $BaseCommit.tree.sha

$Tree = @()
foreach ($File in $Files) {
  $Bytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $File))
  $Content = [Convert]::ToBase64String($Bytes)
  $Blob = Invoke-GhJson "POST" "repos/$Repo/git/blobs" @{
    content = $Content
    encoding = "base64"
  }
  $Tree += @{
    path = ($File -replace "\\", "/")
    mode = "100644"
    type = "blob"
    sha = $Blob.sha
  }
}

$NewTree = Invoke-GhJson "POST" "repos/$Repo/git/trees" @{
  base_tree = $BaseTreeSha
  tree = $Tree
}

$NewCommit = Invoke-GhJson "POST" "repos/$Repo/git/commits" @{
  message = $Message
  tree = $NewTree.sha
  parents = @($BaseCommitSha)
}

Invoke-GhJson "PATCH" "repos/$Repo/git/refs/heads/main" @{
  sha = $NewCommit.sha
  force = $false
} | Out-Null

Write-Output $NewCommit.sha
