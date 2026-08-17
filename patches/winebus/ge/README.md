GE-patчи winebus для build-пути (применяются на ValveSoftware/wine@9578fa3).

Порядок применения build-путём setup.sh:

  1. 0018-winebus-remove-hidraw-device-on-fatal-read-error.patch
  2. 0036-winebus-ignore-duplicate-udev-devnodes.patch
  3. ../v13-sony-gamepad.patch

0014-winebus-prefer-hidraw-for-dualsense-hotplug.patch — СПРАВОЧНЫЙ,
НЕ применяется:

- полный 0014 не ложится на Valve base git-apply (в его контексте
  bus_sdl.c нужен is_emulating_steaminput, которого в base нет);
- в задеплоенном дереве из 0014 взят ТОЛЬКО sdl-блок (игнор
  DualSense в SDL-источнике), на другом якоре — он включён в
  v13-sony-gamepad.patch;
- main.c-часть 0014 (DualSense USB haptics: BT->USB трансляция
  output-репортов, включение haptics-режима) в деплой НЕ входила
  и продуктом не является.
