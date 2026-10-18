@echo off
setlocal
cd /d "%~dp0"
where arduino-cli >nul 2>nul
if errorlevel 1 (echo Install Arduino CLI first: https://arduino.github.io/arduino-cli/ & exit /b 1)
arduino-cli config init --overwrite
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.9
arduino-cli lib install "AsyncTCP"
arduino-cli lib install "ESP Async WebServer"
arduino-cli lib install "ElegantOTA"
arduino-cli compile --fqbn esp32:esp32:esp32 --build-property compiler.cpp.extra_flags="-DELEGANTOTA_USE_ASYNC_WEBSERVER=1" "Autoheater_ESP32_Bootstrap"
if errorlevel 1 exit /b 1
arduino-cli board list
set /p PORT=Enter ESP32 serial port (for example COM3):
arduino-cli upload -p "%PORT%" --fqbn esp32:esp32:esp32 "Autoheater_ESP32_Bootstrap"
exit /b %errorlevel%
