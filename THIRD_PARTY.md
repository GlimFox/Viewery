# Медиазависимости Viewery

`scripts/Get-MediaTools.ps1` скачивает архивы напрямую с GitHub Releases и проверяет зафиксированные SHA-256. Файлы из архивов нужны для локального запуска и проверок. В репозиторий и артефакт сборки входят файлы Viewery, а медиазависимости пользователь загружает отдельно.

| Компонент | Как используется | Исходники и лицензия |
| --- | --- | --- |
| libmpv 0.42.0, Windows x64 | Viewery загружает `libmpv-2.dll` из каталога приложения. Скрипт выбирает архив `libmpv-v0.42.0-x86_64-w64-mingw32-lgpl.zip`, официальный LGPL-вариант Windows-сборки. | [Релиз и исходники mpv](https://github.com/mpv-player/mpv/releases/tag/v0.42.0), [правила лицензирования исходников mpv и зависимостей](https://github.com/mpv-player/mpv/blob/v0.42.0/Copyright), [LGPL версии 3](https://www.gnu.org/licenses/lgpl-3.0.html), [конфигурация CI](https://github.com/mpv-player/mpv/blob/v0.42.0/.github/workflows/build.yml). |
| FFmpeg 9.0.2 Essentials, Windows x64, сборка Gyan | Конвертер запускает отдельный процесс `ffmpeg.exe`. Флаг `-IncludeProbe` дополнительно устанавливает `ffprobe.exe` для проверок. Сборка использует `--enable-gpl --enable-version3` и сообщает лицензию GPL-3.0-or-later через `ffmpeg.exe -L`. | [Релиз Gyan](https://github.com/GyanD/codexffmpeg/releases/tag/9.0.2), [ревизия FFmpeg](https://github.com/FFmpeg/FFmpeg/commit/946fcce07b), [лицензирование FFmpeg](https://ffmpeg.org/legal.html), [GPL версии 3](https://www.gnu.org/licenses/gpl-3.0.html). |

Если готовый пакет Viewery будет распространяться вместе с медиабиблиотеками или исполняемыми файлами, для точных сборок понадобятся применимые тексты лицензий, уведомления об авторских правах, соответствующие исходники и материалы сборки, включая исходники встроенных зависимостей. Для LGPL-сборки следует сохранить возможность заменить `libmpv-2.dll` ABI-совместимой версией.
