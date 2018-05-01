@echo off

set DEVICE_NAME=outline-tap0

:: Check whether the device already exists.
netsh interface show interface name=%DEVICE_NAME% >nul
if %errorlevel% equ 0 (
  echo %DEVICE_NAME% already exists.
  exit /b
)

:: Add the device.
:: TODO: Detect the system's architecture.
tap-windows6\amd64\tapinstall install tap-windows6\amd64\OemVista.inf tap0901 >nul
if %errorlevel% neq 0 (
  echo Could not create TAP device.
  exit /b 1
)

:: Find the name of the new device.
:: If there are other tap-windows6 applications installed, there could
:: be several: assume the one we just created appears *last* in the list.
:: TODO: Compile the driver ourselves, with different default name and IDs.
for /f "tokens=2 delims=:" %%i in ('tap-windows6\amd64\tapinstall hwids tap0901 ^|find "Name:"') do set RESULT=%%i
:: Strip leading whitespace from the variable.
for /f "tokens=* delims= " %%a in ("%RESULT%") do set RESULT=%%a
echo New TAP device name: %RESULT%

:: Now we now the new device's name.
:: However, in order to use netsh on the new device, we needs its *ID*.
::
:: We can use some wmic magic; to see how this works, examine the output of:
::   wmic /output:c:\wmic.csv nic
::
:: Escaping gets comically complicated, some help can be found here:
::   https://stackoverflow.com/questions/15527071/wmic-command-not-working-in-for-loop-of-batch-file
for /f "tokens=2 delims==" %%i in ('wmic nic where "name=\"%RESULT%\"" get netconnectionID /format:list') do set ID=%%i
echo New TAP device ID: %ID%

:: Rename the device.
netsh interface set interface name = "%ID%" newname = "%DEVICE_NAME%" >nul
if %errorlevel% neq 0 (
  echo Could not rename TAP device.
  exit /b 1
)

:: Set ip address, etc.
:: TODO: Automatically find an unused subnet.
netsh interface ip set address %DEVICE_NAME% static 192.168.7.2 255.255.255.0 >nul
if %errorlevel% neq 0 (
  echo Could not set TAP device subnet.
  exit /b 1
)
