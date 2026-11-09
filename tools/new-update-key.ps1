param(
    [string]$Repo = '1Filipek2/island-bridge',
    [string]$BackupPath,
    [switch]$Force
)

# Creates the ECDSA P-384 key that signs update manifests and stores it as the secret
# UPDATE_SIGNING_KEY of the protected "release" environment (tags v* only, owner approval).
# The private key goes straight from memory into gh; it is never printed and only touches disk as a
# password-encrypted backup when -BackupPath is given. Prints the public key for UpdateSignature.TrustedKeys.

$ErrorActionPreference = 'Stop'

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'gh (GitHub CLI) is required' }

$existing = gh secret list --env release --repo $Repo --json name --jq '.[].name' 2>$null
if (($existing -contains 'UPDATE_SIGNING_KEY') -and -not $Force) {
    throw 'UPDATE_SIGNING_KEY already exists; rerun with -Force only for a planned key rotation'
}

$user = gh api user --jq .id
$environment = @{
    reviewers = @(@{ type = 'User'; id = [int64]$user })
    prevent_self_review = $false
    deployment_branch_policy = @{ protected_branches = $false; custom_branch_policies = $true }
} | ConvertTo-Json -Depth 4
$environment | gh api -X PUT "repos/$Repo/environments/release" --input - | Out-Null

$policies = gh api "repos/$Repo/environments/release/deployment-branch-policies" --jq '.branch_policies[] | "\(.type):\(.name)"'
if ($policies -notcontains 'tag:v*') {
    gh api -X POST "repos/$Repo/environments/release/deployment-branch-policies" -f name='v*' -f type=tag | Out-Null
}

$key = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP384)
try {
    if ($BackupPath) {
        $password = Read-Host 'Backup password (long, from your password manager)' -AsSecureString
        $plain = [Net.NetworkCredential]::new('', $password).Password
        if ($plain.Length -lt 16) { throw 'use a backup password of at least 16 characters' }
        $pbe = [Security.Cryptography.PbeParameters]::new([Security.Cryptography.PbeEncryptionAlgorithm]::Aes256Cbc, [Security.Cryptography.HashAlgorithmName]::SHA256, 600000)
        $pem = $key.ExportEncryptedPkcs8PrivateKeyPem($plain, $pbe)
        [IO.File]::WriteAllText($BackupPath, $pem)
        $plain = $null
        Write-Host "Encrypted backup written to $BackupPath"
    }

    $private = [Convert]::ToBase64String($key.ExportPkcs8PrivateKey())
    $private | gh secret set UPDATE_SIGNING_KEY --env release --repo $Repo
    if ($LASTEXITCODE -ne 0) { throw 'gh secret set failed' }
    $private = $null

    $public = [Convert]::ToBase64String($key.ExportSubjectPublicKeyInfo())
}
finally {
    $key.Dispose()
}

Write-Host 'UPDATE_SIGNING_KEY stored in the release environment.'
Write-Host 'Public key for UpdateSignature.TrustedKeys:'
Write-Output $public
