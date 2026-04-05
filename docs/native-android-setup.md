# Окружение разработки нативного Android/Quest 2 приложения

Руководство по установке всех компонентов, необходимых для сборки `fpv-native-quest/`.

## Проверка текущего состояния

```bash
# Java
java -version              # нужен openjdk 17

# NDK
ls ~/Library/Android/sdk/ndk/  # нужна директория 27.x.xxxxxxx

# CMake
ls ~/Library/Android/sdk/cmake/ # нужна версия 3.22.1

# ADB
adb version                # нужен ADB version >= 1.0.41

# Android platforms
ls ~/Library/Android/sdk/platforms/  # нужен android-32
```

---

## Шаг 1 — JDK 17

```bash
brew install openjdk@17
```

Добавить в `~/.zshrc`:

```bash
export JAVA_HOME=/opt/homebrew/opt/openjdk@17
export PATH="$JAVA_HOME/bin:$PATH"
```

Применить и проверить:

```bash
source ~/.zshrc
java -version
# ожидаемый вывод: openjdk version "17.x.x" ...
```

---

## Шаг 2 — Android NDK r27 и CMake 3.22.1

**Android Studio → Settings → SDK Manager → вкладка SDK Tools**

Включить чекбоксы:
- [x] **NDK (Side by side)** → версия **27.x** (последняя 27.2.12479018 или новее)
- [x] **CMake** → версия **3.22.1**

Нажать **Apply** → **OK** → дождаться загрузки.

После установки:

```bash
ls ~/Library/Android/sdk/ndk/
# 27.2.12479018  (или аналогичная)

ls ~/Library/Android/sdk/cmake/
# 3.22.1
```

---

## Шаг 3 — Android API 32 (Android 12L)

**Android Studio → Settings → SDK Manager → вкладка SDK Platforms**

Включить чекбокс:
- [x] **Android 12L (API Level 32)**

Quest 2 работает на Android 10/12; `targetSdk = 32`, `minSdk = 29`.  
Для компиляции нужен `compileSdk = 33` (уже есть `android-36` — подойдёт; API 32 нужен для точной проверки атрибутов манифеста).

---

## Шаг 4 — Настройка Quest 2 для разработки

### 4.1 — Включить Developer Mode

1. Установить приложение **Meta Quest** (iOS / Android)
2. Войти в тот же Meta-аккаунт, к которому привязан шлем
3. **Menu (☰) → Devices** → выбрать Quest 2
4. **Headset Settings → Developer Mode → ON**
5. Перезагрузить шлем (кнопка питания → Restart)

> Developer Mode включается **только через мобильное приложение Meta Quest**.  
> Из меню шлема или браузера ноутбука это сделать нельзя.

### 4.2 — ADB-авторизация

1. Подключить Quest 2 по USB-C
2. Надеть шлем — в нём появится диалог **«Allow USB Debugging?»**
3. Нажать **Allow** (поставить галку «Always allow from this computer»)
4. Проверить:

```bash
adb devices
# 1WMHXXXXXXXX    device   ← статус должен быть "device", не "unauthorized"
```

| Статус | Причина | Решение |
|--------|---------|---------|
| `device` | ✅ всё готово | — |
| `unauthorized` | диалог не принят | надеть шлем, найти диалог, Allow |
| пустой список | диалог не появился | отключить/подключить USB, разблокировать шлем |
| `offline` | проблема с кабелем | другой кабель или USB-порт |

### 4.3 — Добавить ADB в PATH (если ещё не сделано)

```bash
# Добавить в ~/.zshrc:
export ANDROID_HOME="$HOME/Library/Android/sdk"
export PATH="$ANDROID_HOME/platform-tools:$PATH"

source ~/.zshrc
adb version
```

---

## Шаг 5 — Первая сборка проекта

```bash
cd /путь/к/fpv-webrtc/fpv-native-quest/

# Создать local.properties (не коммитится, в .gitignore)
cp local.properties.template local.properties
# Отредактировать sdk.dir:
echo 'sdk.dir=/Users/konstantin/Library/Android/sdk' >> local.properties

# Собрать debug APK
./gradlew assembleDebug
```

Успешный вывод:
```
BUILD SUCCESSFUL in 2m 30s
35 actionable tasks: 35 executed
```

APK создан по пути:
```
app/build/outputs/apk/debug/app-debug.apk
```

---

## Шаг 6 — Установка и запуск на Quest 2

```bash
# Установить (Quest 2 должен быть подключён и разблокирован)
./gradlew installDebug
# или вручную:
adb install app/build/outputs/apk/debug/app-debug.apk

# Запустить через ADB
adb shell am start -n com.fpv.quest/.MainActivity

# Приложение также появится в Quest 2:
# Apps → фильтр "Unknown Sources" → "FPV Quest"
```

---

## Шаг 7 — Просмотр логов

```bash
# Логи FPV приложения (все модули)
adb logcat -s FPVQuest SignalingClient WebRTCEngine FPVDataChannel xr_renderer video_decoder

# Только ошибки
adb logcat *:E

# Очистить буфер и смотреть в реальном времени
adb logcat -c && adb logcat -s FPVQuest
```

---

## Открытие в Android Studio

1. **File → Open** → выбрать директорию `fpv-native-quest/`
2. Android Studio предложит скачать Gradle и синхронизировать проект — нажать **OK**
3. В панели **Device Manager** должен появиться Quest 2
4. Нажать **Run ▶** — сборка и установка на шлем автоматически

> Android Studio автоматически обрабатывает `gradle-wrapper.jar` и настройку Gradle.  
> Для командной строки (`./gradlew`) нужно предварительно установить JDK 17 (Шаг 1).

---

## Устранение проблем сборки

| Ошибка | Причина | Решение |
|--------|---------|---------|
| `No JDK found` | JDK 17 не установлен | Шаг 1 |
| `NDK not configured` | NDK не установлен | Шаг 2 |
| `Cmake '3.22.1' was not found` | CMake не установлен | Шаг 2 |
| `SDK location not found` | `local.properties` отсутствует | Шаг 5 |
| `INSTALL_FAILED_USER_RESTRICTED` | Developer Mode выключен | Шаг 4.1 |
| `INSTALL_FAILED_VERSION_DOWNGRADE` | Более новая версия установлена | `adb uninstall com.fpv.quest` |
| `error: no devices/emulators found` | Quest не подключён или не авторизован | Шаг 4.2 |

---

## Gradle wrapper JAR

Файл `gradle/wrapper/gradle-wrapper.jar` — бинарный, не включён в репозиторий.  
Он загружается автоматически при первом открытии в Android Studio.  
Для командной строки без Android Studio:

```bash
# Способ 1: если установлен Gradle CLI
brew install gradle
gradle wrapper --gradle-version=8.7

# Способ 2: скачать вручную
mkdir -p gradle/wrapper
curl -L -o gradle/wrapper/gradle-wrapper.jar \
  "https://github.com/gradle/gradle/raw/v8.7.0/gradle/wrapper/gradle-wrapper.jar"
```

---

## Следующие шаги

После успешной сборки и установки (чёрный экран в шлеме = TASK-002 выполнена):

- **TASK-003** — SignalingClient + WebRTCEngine: первый видеопоток в шлеме
- **TASK-004** — OpenXR + xr_renderer: стерео VR рендеринг
- **TASK-005** — FPVDataChannel: E2E метрики из нативного приложения
