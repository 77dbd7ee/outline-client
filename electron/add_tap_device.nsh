; Adds a tap-windows6 device to the system.

!macro customInstall
  ; MessageBox MB_OK "doing the TAP dance"
  File /r "${PROJECT_DIR}\tap-windows6"
  File "${PROJECT_DIR}\electron\add_tap_device.bat"
  ExecWait 'add_tap_device.bat'
  ; TODO: handle script failure
!macroend

; TODO: uninstaller
