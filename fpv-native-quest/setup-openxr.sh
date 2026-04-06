#!/usr/bin/env bash
# setup-openxr.sh — Установить хедеры OpenXR для автодополнения в IDE.
#
# ╔══════════════════════════════════════════════════════════════════╗
# ║  Для СБОРКИ APK этот скрипт НЕ нужен.                          ║
# ║  Gradle скачивает OpenXR loader автоматически с Maven Central:  ║
# ║    org.khronos.openxr:openxr_loader_for_android:1.1.49          ║
# ║  CMake получает хедеры и .so через Prefab AAR.                  ║
# ╚══════════════════════════════════════════════════════════════════╝
#
# Запускай этот скрипт если Android Studio или CLion не индексирует
# хедеры OpenXR из кешированного Gradle AAR:
#   cd fpv-native-quest/
#   ./setup-openxr.sh
#
# Хедеры устанавливаются в: third_party/openxr/include/openxr/
# После этого: File → Sync Project with Gradle Files в Android Studio.
#
# Требования: curl, tar
# Лицензия хедеров: Apache 2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INCLUDE_DIR="${SCRIPT_DIR}/third_party/openxr/include/openxr"

# Версия совпадает с зависимостью в app/build.gradle
OPENXR_VERSION="1.1.49"
OPENXR_TAG="release-${OPENXR_VERSION}"
OPENXR_URL="https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/${OPENXR_TAG}.tar.gz"

echo "=== FPV-Native: установка хедеров OpenXR ${OPENXR_VERSION} ==="
echo ""
echo "Источник: ${OPENXR_URL}"
echo "Назначение: ${INCLUDE_DIR}"
echo ""

mkdir -p "${INCLUDE_DIR}"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

echo "Скачиваю архив..."
curl -fSL "${OPENXR_URL}" -o "${TMP_DIR}/openxr-sdk.tar.gz"

echo "Распаковываю хедеры..."
tar -xzf "${TMP_DIR}/openxr-sdk.tar.gz" \
    -C "${TMP_DIR}" \
    --strip-components=3 \
    "OpenXR-SDK-${OPENXR_TAG}/include/openxr"

cp "${TMP_DIR}"/*.h "${INCLUDE_DIR}/"

echo ""
echo "Установлены файлы:"
ls "${INCLUDE_DIR}"
echo ""
echo "Готово. Перезапусти индексацию: File → Sync Project with Gradle Files."
echo ""
echo "Примечание: loader (libopenxr_loader.so) поставляется через Gradle Prefab AAR —"
echo "  org.khronos.openxr:openxr_loader_for_android:${OPENXR_VERSION} (Maven Central)"
echo "  Обновить версию: app/build.gradle → implementation строка"
