# =========================================
# H-EXO Omni-Core: Auto setup CPU-only models
# =========================================

Write-Host ">>> Starting H-EXO model setup..." -ForegroundColor Cyan

# -----------------------------
# 1️⃣ Путь к Python (автоматически)
# -----------------------------
$pythonPath = (py -0p | ForEach-Object { ($_ -split '\s+')[1] }) 
if (-not (Test-Path $pythonPath)) {
    Write-Host "❌ Python не найден! Скачайте Python 3.x и добавьте в PATH или используйте py launcher." -ForegroundColor Red
    exit
} else { Write-Host "✅ Python найден: $pythonPath" }

# -----------------------------
# 2️⃣ Папка для моделей
# -----------------------------
$modelsDir = "$env:USERPROFILE\HExo\models"
New-Item -ItemType Directory -Force -Path $modelsDir | Out-Null

# -----------------------------
# 3️⃣ Список моделей и ссылки (проверенные GGML)
# -----------------------------
$models = @{
    "llama-2-7b-chat-q4_0.bin" = "https://huggingface.co/TheBloke/llama-2-7b-chat-GGML/resolve/main/llama-2-7b-chat-GGML-q4_0.bin"
    "mpt-7b-instruct-q4_0.bin" = "https://huggingface.co/TheBloke/MPT-7B-Instruct-GGML/resolve/main/mpt-7b-instruct-GGML-q4_0.bin"
    "falcon-7b-instruct-q4_0.bin" = "https://huggingface.co/TheBloke/Falcon-7B-Instruct-GGML/resolve/main/falcon-7b-instruct-GGML-q4_0.bin"
}

# -----------------------------
# 4️⃣ Проверка и скачивание моделей
# -----------------------------
foreach ($model in $models.Keys) {
    $filePath = Join-Path $modelsDir $model
    if (-Not (Test-Path $filePath)) {
        Write-Host "📥 Модель $model не найдена. Скачиваем..."
        try {
            Invoke-WebRequest -Uri $models[$model] -OutFile $filePath -UseBasicParsing
            Write-Host "✅ $model скачана успешно"
        }
        catch {
            Write-Host "❌ Ошибка при скачивании $model. Скачайте вручную: $($models[$model])" -ForegroundColor Red
        }
    }
    else {
        Write-Host "✅ Модель $model уже существует"
    }
} # <-- Закрываем foreach

# -----------------------------
# 5️⃣ Создание Continue config.json
# -----------------------------
$continuePath = "$env:USERPROFILE\.continue\config.json"
New-Item -ItemType Directory -Force -Path "$env:USERPROFILE\.continue" | Out-Null

$continueConfig = @{
    models = @(
        @{
            title = "LLaMA2-7B-Chat"
            provider = "local"
            model = "llama-2-7b-chat-q4_0"
            path = Join-Path $modelsDir "llama-2-7b-chat-q4_0.bin"
        },
        @{
            title = "MPT-7B-Instruct"
            provider = "local"
            model = "mpt-7b-instruct-q4_0"
            path = Join-Path $modelsDir "mpt-7b-instruct-q4_0.bin"
        },
        @{
            title = "Falcon-7B-Instruct"
            provider = "local"
            model = "falcon-7b-instruct-q4_0"
            path = Join-Path $modelsDir "falcon-7b-instruct-q4_0.bin"
        }
    )
    defaultModel = "LLaMA2-7B-Chat"
    completionOptions = @{
        temperature = 0.05
        maxTokens = 8192
    }
    tools = @(
        @{ name="read_pmu"; description="Read CPU PMU counters" },
        @{ name="dump_memory"; description="Dump raw memory" },
        @{ name="uart_cmd"; description="Send UART debug command" }
    )
}

$continueConfig | ConvertTo-Json -Depth 10 | Set-Content $continuePath -Encoding UTF8
Write-Host "✅ Continue config.json создан успешно!"

# -----------------------------
# 6️⃣ Итог
# -----------------------------
Write-Host ">>> Все модели проверены и подключены. Путь к моделям:" -ForegroundColor Green
Get-ChildItem $modelsDir | ForEach-Object { Write-Host "   - $($_.Name)" }

Write-Host ">>> Запуск Continue теперь будет использовать локальные модели CPU-only" -ForegroundColor Green