# Autoheater ESP32 — Bootstrap

Стартовая прошивка с ElegantOTA для первоначальной записи по USB на ESP32 DevKit / ESP32-WROOM-32.

- Исходник: `Autoheater_ESP32_Bootstrap/Autoheater_ESP32_Bootstrap.ino`
- Скрипт Windows: `flash_bootstrap.bat`
- Wi-Fi: `Autoheater`, пароль `12345678`
- OTA: `http://192.168.4.1/update`, логин/пароль `admin` / `admin`

Бинарник приложения для OTA не является полным образом первичной прошивки — не записывайте его по адресу 0x0. Основное приложение и bootstrap должны использовать совместимые платы и схемы разделов. Указанные учётные данные предназначены только для стенда.
