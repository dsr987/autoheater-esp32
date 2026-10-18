# Процесс сборки и выпуска

GitHub Actions собирает основное приложение и bootstrap на push/PR. Артефакт `autoheater-esp32-firmware` содержит OTA-бинарник приложения, SHA-256 и бинарник bootstrap. Успешная сборка основной ветки публикуется как GitHub Release с тегом `firmware-vX.Y.Z`, версия читается из `CHANGELOG_VERSION` в `firmware/app/app.cpp`.

Метки выпуска создаются запуском Actions → Release label setup → Run workflow:

- `release:patch` — исправление;
- `release:minor` — совместимое расширение;
- `release:major` — несовместимое изменение после 1.0.0;
- `release:stable` — переход с 0.x на 1.0.0;
- `release:none` — документация, исследование или автоматизация без выпуска.

Пока метки создаются автоматически, но повышение версии по метке PR ещё не подключено в этом стартовом каркасе.
