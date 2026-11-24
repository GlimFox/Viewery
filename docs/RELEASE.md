# Сборка и обновление Windows-релиза

Состояние на 24 ноября 2026: есть воспроизводимая сборка portable ZIP и полного MSIX, код проверки GitHub Releases и установки через Windows App Installer. Сертификат цифровой подписи не предоставлен. Подписанная первая установка и переход между двумя установленными версиями ещё не проверены. Unsigned MSIX является артефактом упаковки, его нельзя представлять как готовый доверенный установщик.

## Portable beta

На машине с Visual Studio C++ x64 и Windows SDK:

```powershell
cmake -S . -B build-release -G "Visual Studio 17 2022" -A x64 "-DVIEWERY_VERSION=0.2.0-beta.6" -DVIEWERY_MEDIA_TESTS=ON
cmake --build build-release --config Release --parallel
./scripts/Get-MediaTools.ps1 -Destination build-release/bin/Release -IncludeProbe
ctest --test-dir build-release -C Release --output-on-failure
./scripts/Build-Release.ps1 -BuildDirectory build-release/bin/Release -Destination dist -Version '0.2.0-beta.6'
```

Пакет `Viewery-0.2.0-beta.6-Windows-x64-portable.zip` содержит только приложение, shell DLL, README, CHANGELOG, инструкции, лицензии и скрипт с проверяемыми SHA-256. Пользователь распаковывает пакет в постоянный каталог, запускает `Viewery.exe` и выбирает установку медиакомпонентов в меню. Для классического ПКМ и фонового Quick View запустите скрипт интеграции Проводника по инструкции README. Компоненты устанавливаются в `%LOCALAPPDATA%\Viewery\MediaTools`, данные коллекции — в `%LOCALAPPDATA%\Viewery`, настройки — в `HKCU\Software\Viewery`.

Аргументы CMake с номером версии передавайте целиком в кавычках, включая `"-DCMAKE_SYSTEM_VERSION=10.0.26100.0"` при явном выборе SDK. Иначе PowerShell может передать неполное значение. CMake проверяет формат версии приложения и диапазон компонентов MSIX `0..65535`; неверный кеш исправляется повторной конфигурацией с `"-DVIEWERY_VERSION=0.2.0-beta.6"` до сборки и упаковки.

Portable проверяет наличие версии и показывает заметки релиза. Автоматическая замена portable EXE не выполняется. Установка обновления из приложения доступна для подписанной версии, установленной через `.appinstaller`.

## Подписанный полный MSIX

1. Предоставить доверенный Windows сертификат подписи кода с приватным ключом и Subject `CN=Viewery`. Сохранить эту идентичность для следующих версий. Для иного Subject нужно согласованно поменять `packaging/AppxManifest.xml`, `src/Viewery.manifest`, updater и скрипт упаковки до первой публичной установки.
2. Вручную проверить происхождение сертификата и ограничить доступ к приватному ключу. Скрипты Viewery не создают сертификаты и не добавляют их в доверенные корневые хранилища.
3. Для локальной сборки сертификат должен быть доступен в `Cert:\CurrentUser\My`. Передать его thumbprint:

```powershell
./scripts/Build-Release.ps1 -BuildDirectory build-release/bin/Release -Destination dist-signed -Version '0.2.0-beta.6' -SigningThumbprint YOUR_40_DIGIT_THUMBPRINT
```

Скрипт подписывает и проверяет EXE, shell DLL и MSIX через SignTool с SHA-256 и RFC 3161 timestamp. Он сохраняет подпись в готовом portable ZIP и генерирует рабочий `Viewery.appinstaller` только после успешной подписи. MakeAppx выполняет проверку схемы и содержимого, `/nv` не используется. При отсутствии сертификата создаются `.unsigned.msix` и `.appinstaller.template.xml`.

Полный MSIX имеет идентичность `Viewery.Explorer` и содержит все файлы приложения. Он добавляет пункт современного ПКМ Windows 11 через `IExplorerCommand`/COM и приложение в меню «Пуск». Идентичность совпадает с прежним sparse package. Старую экспериментальную регистрацию нужно удалить перед первой полноценной установкой, если она была подписана другим сертификатом. Данные `%LOCALAPPDATA%\Viewery` остаются вне каталога установки и не удаляются обновлением. Для этих путей отключена MSIX-виртуализация реестра и файлов.

Семантические версии `0.2.0-beta.N` отображаются в MSIX как `0.2.0.N`, а `0.2.0` как `0.2.0.65535`. Следующая стабильная версия должна увеличить один из первых трёх компонентов. Пользовательская `-PackageVersion` допускается для проверяемой последовательности, однако нельзя понижать номер уже опубликованного канала.

## GitHub Actions и каналы

`release.yml` запускается при push тега `v*` или вручную для уже существующего тега. Он собирает Windows x64, скачивает pinned-медиа только для тестов, выполняет CTest, упаковывает приложение и публикует неизменяемый релиз версии. Повторная публикация существующего релиза завершается ошибкой, файлы версии не перезаписываются.

Оба workflow явно выбирают MSVC v143 и Windows SDK `10.0.26100.0` на `windows-2022`. C++/WinRT берётся из этого SDK, `WindowsApp.lib` и `runtimeobject.lib` связаны с приложением и тестом updater. Дополнительный Windows App SDK runtime не требуется.

Текст релиза извлекается из точной секции версии в `CHANGELOG.md` скриптом `Get-ReleaseNotes.ps1` и сохраняется как UTF-8 в `Viewery-<версия>-ReleaseNotes.md`. Публикация передаёт этот файл в `gh release create --notes-file`; отсутствие секции останавливает упаковку. Workflow `windows.yml` использует тот же скрипт полной упаковки с проверкой MakeAppx, поэтому pull request проверяет и состав релиза.

Без signing secrets публикуются portable ZIP и `SHA256SUMS.txt`. Unsigned MSIX и шаблон AppInstaller остаются Actions artifact для проверки. При наличии секретов `WINDOWS_SIGNING_PFX_BASE64` и `WINDOWS_SIGNING_PFX_PASSWORD` PFX временно импортируется только в личное хранилище ключей одноразового runner, удаляется в `finally`, а доверенные корневые хранилища не меняются. Для production настройте окружению `release` обязательное ручное ревью и защитите теги/ветки от посторонней записи.

После успешной подписи workflow также публикует:

- Неизменяемый пакет `releases/download/v0.2.0-beta.6/Viewery-0.2.0-beta.6-x64.msix`.
- Канал `releases/download/updates-beta/Viewery.appinstaller` либо `updates-stable`.

GitHub `latest` не используется для beta. Канальный релиз содержит только текущий AppInstaller; содержимое `.appinstaller` указывает на конкретную подписанную версию. Канальный тег не является семантической версией и игнорируется проверкой наличия обновления.

## Проверка и применение обновления

Проверка выполняется рабочим потоком с timeout и ограничением размера ответа GitHub API. Установленная stable-версия не предлагает prerelease. Наличие более новой версии показывает уведомление, номер и заметки релиза. Флажок автоматической проверки хранится в `HKCU\Software\Viewery\AutomaticUpdateChecks`; ручная проверка остаётся доступна независимо от него. Проверка обновлений не блокирует открытие Quick View.

После подтверждения и завершения конвертации/переименования updater получает пакет через `PackageManager.FindPackageForUser`, проверяет `CheckUpdateAvailabilityAsync`, идентичность и разрешённый HTTPS-канал. Для установки используется `RequestAddPackageByAppInstallerFileAsync`. Windows проверяет подпись и целостность пакета; ошибка загрузки или подписи не заменяет текущую установленную версию. Запрос с `ForceTargetAppShutdown` может закрыть процесс, поэтому UI обязан закончить операции до вызова и предупредить о перезапуске.

В `.appinstaller` отсутствуют системные `OnLaunch` и `AutomaticBackgroundTask`: приложение само управляет проверкой и моментом установки. Это позволяет отложить обновление и не прерывать операции с файлами. Метаданные хранятся вне MSIX, миграции и резервная копия выполняются модулем коллекции.

## Обязательная проверка перед выпуском доверенного установщика

На отдельной Windows 11 VM установить настоящую подписанную версию N через опубликованный `.appinstaller`, создать коллекцию, метаданные, пресет и запись истории. Опубликовать подписанную N+1 тем же издателем и повысить версию канала. Проверить уведомление, заметки, «Позже», ручную/отключённую автоматическую проверку, установку при отсутствии активных операций, повторный запуск и сохранение данных. Отдельно проверить отсутствие сети, повреждённый пакет, отказ Windows в подписи и сохранение работоспособной N после ошибки. Проверить запуск из «Пуск», Space в Проводнике и современный ПКМ после обновления.

Этот сценарий не заменяется тестом парсера версий или успешной сборкой MSIX. До появления сертификата результат следует указывать как «упаковка и код обновления проверены сборкой; E2E установки/обновления заблокирован отсутствием сертификата».

## Первичные документы

- [Windows App Installer APIs](https://learn.microsoft.com/en-us/windows/msix/app-installer/app-installer-documentation).
- [Обновление из кода приложения](https://learn.microsoft.com/en-us/windows/msix/non-store-developer-updates).
- [Необязательный UpdateSettings](https://learn.microsoft.com/en-us/uwp/schemas/appinstallerschema/element-update-settings).
- [AppInstaller schema](https://learn.microsoft.com/en-us/uwp/schemas/appinstallerschema/schema-root).
- [MSIX подпись SignTool](https://learn.microsoft.com/en-us/windows/msix/package/sign-app-package-using-signtool).
