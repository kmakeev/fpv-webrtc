# Установка APK на Quest 2

Руководство по установке нативного приложения `fpv-native-quest` на Oculus Quest 2
без публикации в Meta Quest Store (sideloading).

---

## Требования

- Quest 2 с включённым **Developer Mode** (см. ниже)
- APK-файл: `fpv-native-quest/app/build/outputs/apk/debug/app-debug.apk`
  или скачанный из раздела [Releases](https://github.com/kmakeev/fpv-webrtc/releases)

---

## Шаг 0 — Включить Developer Mode на Quest 2

1. Установить **Meta Quest** приложение на смартфон
2. Войти в тот же аккаунт Meta, что и на шлеме
3. В приложении: **Меню → Устройства → выбрать свой Quest 2**
4. **Настройки устройства → Developer Mode → включить**
5. Надеть шлем — появится запрос на разрешение USB debugging → **Allow**

> Developer Mode нужно включить только один раз. После этого sideload работает и по USB, и по Wi-Fi.

---

## Вариант A — ADB через USB (быстро, надёжно)

### Установка ADB

**macOS:**
```bash
brew install android-platform-tools
```

**Windows:**
Скачать [Platform Tools](https://developer.android.com/tools/releases/platform-tools) и распаковать.
Добавить папку в `PATH`.

**Linux:**
```bash
sudo apt install adb
```

### Установка APK

```bash
# Подключить Quest 2 USB-C кабелем к компьютеру
# В шлеме подтвердить запрос "Allow USB Debugging"

# Проверить что устройство видно:
adb devices
# Ожидаемый вывод: 1WMHH8xxxxxxxx    device

# Установить APK:
adb install -r app-debug.apk

# Запустить сразу:
adb shell am start -n com.fpv.quest/.MainActivity

# Смотреть логи в реальном времени (опционально):
adb logcat -s FPVQuest WebRTCEngine SignalingClient xr_renderer
```

Флаг `-r` — переустановка поверх существующего (данные сохраняются).

### Поиск приложения в шлеме после установки

Меню → **Приложения** → в правом верхнем углу выбрать категорию **"Unknown Sources"** (Неизвестные источники).

---

## Вариант B — ADB по Wi-Fi (без кабеля, Quest 2 OS 41+)

```bash
# Один раз — подключить USB и включить беспроводной режим:
adb tcpip 5555
adb connect <IP_Quest>:5555
# IP Quest: Настройки шлема → Wi-Fi → нажать на сеть → показывается IP

# Отключить USB. Дальше работаем по Wi-Fi:
adb install -r app-debug.apk
```

> Беспроводной ADB сбрасывается при перезагрузке Quest. Команду `adb connect` нужно повторять.

---

## Вариант C — Meta Quest Developer Hub (MQDH, графический интерфейс)

Удобен если не хочется работать с командной строкой.

1. Скачать [Meta Quest Developer Hub](https://developer.oculus.com/meta-quest-developer-hub/) и установить
2. Подключить Quest по USB или Wi-Fi (MQDH ищет устройства автоматически)
3. Перейти в раздел **Device Manager → Apps**
4. Нажать **Install App** → выбрать `app-debug.apk`

MQDH также показывает логи, скриншоты и управляет приложениями.

---

## Вариант D — SideQuest (популярный sideload-менеджер)

1. Скачать [SideQuest](https://sidequestvr.com/setup-howto) (Desktop App)
2. Подключить Quest по USB
3. В SideQuest: нажать значок APK в верхнем меню → выбрать файл
4. Дождаться надписи "Successfully installed"

---

## Сборка APK из исходников

Если хочешь собрать APK самостоятельно — следуй инструкции в
[docs/native-android-setup.md](native-android-setup.md), затем:

```bash
cd fpv-native-quest/
export JAVA_HOME=/opt/homebrew/opt/openjdk@17   # macOS Homebrew
./gradlew assembleDebug
# APK: app/build/outputs/apk/debug/app-debug.apk
```

### Подписанный release APK (опционально)

Debug APK полностью функционален на Quest. Release-сборка нужна только
если планируешь распространять APK через сторонние каналы или Meta Store.

**1. Создать keystore (один раз):**

```bash
keytool -genkey -v -keystore fpv-quest-release.jks \
  -alias fpv -keyalg RSA -keysize 2048 -validity 10000 \
  -dname "CN=FPV Quest, O=Personal, C=RU"
```

**2. Добавить блок `signingConfigs` в [app/build.gradle](../fpv-native-quest/app/build.gradle):**

```groovy
android {
    signingConfigs {
        release {
            storeFile     file("/путь/до/fpv-quest-release.jks")
            storePassword "твой_пароль"
            keyAlias      "fpv"
            keyPassword   "твой_пароль"
        }
    }
    buildTypes {
        release {
            signingConfig  signingConfigs.release
            minifyEnabled  true
            proguardFiles  getDefaultProguardFile('proguard-android-optimize.txt'), 'proguard-rules.pro'
        }
    }
}
```

> Не коммить `build.gradle` с паролями в репозиторий. Используй переменные окружения
> или файл `local.properties` (он уже в `.gitignore`).

**3. Собрать и установить:**

```bash
./gradlew assembleRelease
adb install -r app/build/outputs/apk/release/app-release.apk
```

---

## Обновление приложения

Просто повтори установку с флагом `-r` — данные (настроенный URL) сохранятся:

```bash
adb install -r app-debug.apk
```

Без `-r` (или через MQDH/SideQuest) приложение переустанавливается с нуля.
