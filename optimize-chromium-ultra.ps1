# Optimized Chromium Copy Script - <1GB
$SOURCE = "F:\chromium\src\out\Release"
$DEST = "C:\MyBrowser\browser"

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host " Optimized Chromium Copy (<1GB)" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host ""

# Remove old browser folder
if (Test-Path $DEST) {
    Write-Host "Removing old browser..." -ForegroundColor Yellow
    Remove-Item $DEST -Recurse -Force
}

# Create new folder
New-Item -ItemType Directory -Path $DEST -Force | Out-Null

Write-Host "Copying optimized files..." -ForegroundColor Cyan
Write-Host ""

# Copy chrome.exe
Write-Host "[1/8] Copying chrome.exe..." -ForegroundColor Cyan
Copy-Item "$SOURCE\chrome.exe" "$DEST\" -Force
Write-Host "  chrome.exe copied" -ForegroundColor Green

# Copy DLL files
Write-Host "[2/8] Copying DLL files..." -ForegroundColor Cyan
$dlls = Get-ChildItem "$SOURCE\*.dll" -File
foreach ($dll in $dlls) {
    Copy-Item $dll.FullName "$DEST\" -Force
}
Write-Host "  Copied $($dlls.Count) DLL files" -ForegroundColor Green

# Copy PAK files
Write-Host "[3/8] Copying PAK files..." -ForegroundColor Cyan
$paks = Get-ChildItem "$SOURCE\*.pak" -File
foreach ($pak in $paks) {
    Copy-Item $pak.FullName "$DEST\" -Force
}
Write-Host "  Copied $($paks.Count) PAK files" -ForegroundColor Green

# Copy BIN files
Write-Host "[4/8] Copying BIN files..." -ForegroundColor Cyan
$bins = Get-ChildItem "$SOURCE\*.bin" -File
foreach ($bin in $bins) {
    Copy-Item $bin.FullName "$DEST\" -Force
}
Write-Host "  Copied $($bins.Count) BIN files" -ForegroundColor Green

# Copy MANIFEST files (important for side-by-side configuration!)
Write-Host "[4.5/8] Copying MANIFEST files..." -ForegroundColor Cyan
$manifests = Get-ChildItem "$SOURCE\*.manifest" -File
foreach ($manifest in $manifests) {
    Copy-Item $manifest.FullName "$DEST\" -Force
}
if ($manifests.Count -gt 0) {
    Write-Host "  Copied $($manifests.Count) MANIFEST files" -ForegroundColor Green
}

# Copy DAT files
Write-Host "[5/8] Copying DAT files..." -ForegroundColor Cyan
if (Test-Path "$SOURCE\icudtl.dat") {
    Copy-Item "$SOURCE\icudtl.dat" "$DEST\" -Force
    Write-Host "  icudtl.dat copied" -ForegroundColor Green
}

# Copy main locales only
Write-Host "[6/8] Copying locales (optimized)..." -ForegroundColor Cyan
if (Test-Path "$SOURCE\locales") {
    $mainLocales = @(
        # Tiếng Anh & Tiếng Việt (Cơ bản)
        "en-US.pak", "en-GB.pak", "vi.pak", 
        # Châu Âu & Châu Mỹ
        "fr.pak", "de.pak", "es.pak", "it.pak", "ru.pak", "pt-BR.pak", "pt-PT.pak", "nl.pak", "pl.pak", "tr.pak",
        # Châu Á & Các ngôn ngữ phổ biến khác
        "ja.pak", "zh-CN.pak", "zh-TW.pak", "ko.pak", "ar.pak", "hi.pak", "id.pak", "th.pak"
    )
    
    New-Item -ItemType Directory -Path "$DEST\locales" -Force | Out-Null
    
    foreach ($locale in $mainLocales) {
        $sourceFile = "$SOURCE\locales\$locale"
        if (Test-Path $sourceFile) {
            Copy-Item $sourceFile "$DEST\locales\" -Force
            Write-Host "    + $locale" -ForegroundColor Green
        }
    }
}

# Copy resources
Write-Host "[7/8] Copying resources..." -ForegroundColor Cyan
if (Test-Path "$SOURCE\resources") {
    Copy-Item "$SOURCE\resources" "$DEST\" -Recurse -Force
    Write-Host "  resources folder copied" -ForegroundColor Green
}

# Copy swiftshader
Write-Host "[8/8] Copying swiftshader..." -ForegroundColor Cyan
if (Test-Path "$SOURCE\swiftshader") {
    Copy-Item "$SOURCE\swiftshader" "$DEST\" -Recurse -Force
    Write-Host "  swiftshader folder copied" -ForegroundColor Green
}

# Calculate and report size
Write-Host ""
Write-Host "=====================================" -ForegroundColor Green
Write-Host " Optimization Complete!" -ForegroundColor Green
Write-Host "=====================================" -ForegroundColor Green
Write-Host ""

$totalSize = (Get-ChildItem $DEST -Recurse | Measure-Object -Property Length -Sum).Sum
$sizeMB = [math]::Round($totalSize / 1MB, 2)
$sizeGB = [math]::Round($totalSize / 1GB, 2)

Write-Host "Browser folder: $DEST" -ForegroundColor Yellow
Write-Host "Total size: $sizeMB MB (~$sizeGB GB)" -ForegroundColor Green
Write-Host ""

if ($totalSize -lt 1073741824) {
    Write-Host "SUCCESS: Under 1GB target!" -ForegroundColor Green
} else {
    Write-Host "WARNING: Over 1GB (but still optimized)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Ready to use: C:\MyBrowser\browser\chrome.exe" -ForegroundColor Cyan