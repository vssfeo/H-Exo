"# analyze_kernel_structure.ps1 - Анализ структуры ядра H-Exo

Write-Host \"=== АНАЛИЗ СТРУКТУРЫ ЯДРА H-EXO ===\" -ForegroundColor Green
Write-Host \"\"

# Анализ размера файлов
Write-Host \"Размеры ключевых файлов:\" -ForegroundColor Yellow

$keyFiles = @(
    \"kernel_neuro.bin\",
    \"kernel_neuro.elf\",
    \"main_neuro.c\",
    \"boot.s\",
    \"mmu.s\",
    \"linker.ld\"
)

foreach ($file in $keyFiles) {
    if (Test-Path $file) {
        $size = (Get-Item $file).Length
        Write-Host \"  $file : $size байт\" -ForegroundColor Cyan
    }
}

Write-Host \"\"
Write-Host \"Структура папок:\" -ForegroundColor Yellow

# Анализ структуры core/
if (Test-Path \"core\") {
    Write-Host \"core/ :\" -ForegroundColor Gray
    Get-ChildItem \"core\" | ForEach-Object {
        $size = $_.Length
        Write-Host \"  $($_.Name) : $size байт\" -ForegroundColor Gray
    }
}

# Анализ структуры neuro/
if (Test-Path \"neuro\") {
    Write-Host \"neuro/ :\" -ForegroundColor Gray
    Get-ChildItem \"neuro\" | ForEach-Object {
        $size = $_.Length
        Write-Host \"  $($_.Name) : $size байт\" -ForegroundColor Gray
    }
}

# Анализ структуры hal/
if (Test-Path \"hal\") {
    Write-Host \"hal/ :\" -ForegroundColor Gray
    Get-ChildItem \"hal\" | ForEach-Object {
        $size = $_.Length
        Write-Host \"  $($_.Name) : $size байт\" -ForegroundColor Gray
    }
}

Write-Host \"\"
Write-Host \"Анализ функций:\" -ForegroundColor Yellow

# Подсчет функций в ключевых файлах
function Count-Functions {
    param([string]$file)
    
    if (Test-Path $file) {
        $content = Get-Content $file -Raw
        # Ищем функции (определения)
        $functions = ($content | Select-String -Pattern \"^.*\\([^*].*\\)\" -AllMatches).Matches.Count
        # Ищем статические функции
        $staticFunctions = ($content | Select-String -Pattern \"^static .*\\(\" -AllMatches).Matches.Count
        return @{
            Total = $functions
            Static = $staticFunctions
            Regular = ($functions - $staticFunctions)
        }
    }
    return @{Total = 0; Static = 0; Regular = 0}
}

$coreFiles = @(
    \"core/heartbeat.c\",
    \"core/logger.c\",
    \"core/slab.c\",
    \"core/chaos.c\",
    \"neuro/neuro_sync.c\",
    \"neuro/telemetry.c\",
    \"neuro/weight_validation.c\",
    \"hal/uart.c\"
)

foreach ($file in $coreFiles) {
    if (Test-Path $file) {
        $stats = Count-Functions $file
        Write-Host \"  $($file): $($stats.Total) функций ($($stats.Static) static)\" -ForegroundColor Gray
    }
}

Write-Host \"\"
Write-Host \"Анализ нейросети:\" -ForegroundColor Yellow

# Анализ нейронных весов
$neuroSyncContent = Get-Content \"neuro/neuro_sync.c\" -Raw
$weightsCount = ($neuroSyncContent | Select-String -Pattern \"INT_TO_FIXED\").Count
Write-Host \"  Нейронных весов: $weightsCount\" -ForegroundColor Cyan

# Размер весов в байтах
$weightsSize = $weightsCount * 4  # 4 байта на fixed_t
Write-Host \"  Размер весов: $weightsSize байт\" -ForegroundColor Cyan

# Анализ размера таблицы CRC32
$crcContent = Get-Content \"neuro/weight_validation.c\" -Raw
$crcTableSize = 256 * 4  # 256 элементов по 4 байта
Write-Host \"  Таблица CRC32: $crcTableSize байт\" -ForegroundColor Cyan

Write-Host \"\"
Write-Host \"ВОЗМОЖНОСТИ ОПТИМИЗАЦИИ:\" -ForegroundColor Yellow
Write-Host \"1. Нейронные веса: $weightsSize байт (критично, сложная оптимизация)\" -ForegroundColor Gray
Write-Host \"2. Таблица CRC32: $crcTableSize байт (можно алгоритмически)\" -ForegroundColor Gray
Write-Host \"3. Стек: 64КБ (можно уменьшить до 16КБ)\" -ForegroundColor Gray
Write-Host \"4. Строки дебага: ~2-3КБ (можно минимизировать)\" -ForegroundColor Gray
Write-Host \"5. Неиспользуемые функции: потенциально 1-2КБ\" -ForegroundColor Gray
Write-Host \"6. Выравнивание секций: до 1КБ\" -ForegroundColor Gray

Write-Host \"\"
Write-Host \"ПОТЕНЦИАЛЬНАЯ ЭКОНОМИЯ: 10-20КБ\" -ForegroundColor Green
Write-Host \"\"