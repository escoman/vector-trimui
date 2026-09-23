# v06x для TrimUI Brick Pro / CrossMix-OS

Кросс-сборка эмулятора Вектор-06Ц (`v06x`) под AArch64 и упаковка готовой
папки, содержимое которой просто копируется на SD-карту устройства.

Порт — standalone SDL2-приложение: **без libretro, без OpenGL**, видео через
обычный `SDL_Renderer`, звук через SDL2 (48000 Гц), управление геймпадом.

---

## 1. Требования

Сборка идёт **внутри Docker-контейнера** (образ `v06x-trimui-cross`), поэтому
на хосте нужен только Docker. Всё остальное (кросс-компилятор
`aarch64-linux-gnu`, SDL2 2.0.12 и Boost 1.75.0) образ собирает сам.

Хост: Linux x86_64, ~5 ГБ свободного места, доступ в интернет (первая сборка
образа качает и компилирует SDL2 и Boost — это 10–20 минут, последующие
сборки — 1–2 минуты).

### Установка Docker

**Вариант A — Docker CE из официального репозитория (рекомендуется).**
При таком способе создаётся группа `docker`, и контейнеры можно запускать без
`sudo`:

```bash
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker "$USER"
newgrp docker          # или перезайти в сессию
docker info >/dev/null && echo "docker OK, без sudo"
```

**Вариант B — Docker из snap** (`sudo snap install docker`).
snap **не создаёт группу `docker`**, поэтому `usermod -aG docker "$USER"`
выдаст «группа «docker» не существует», а команды будут падать с
`permission denied ... /var/run/docker.sock`. В этом случае:

- просто запускайте сборку с `sudo` — `make.sh` сам определит, что нужен
  `sudo docker`, и подставит его;
- после сборки верните права на результат себе:
  ```bash
  cd platform/trimui
  sudo chown -R "$USER:$USER" build release
  ```
- при желании группу можно создать вручную (срабатывает не на всех версиях
  snap): `sudo groupadd docker && sudo snap restart docker &&
  sudo usermod -aG docker "$USER"`.

---

## 2. Сборка

Из корня репозитория:

```bash
cd platform/trimui
./make.sh
```

Что делает `make.sh`:

1. выбирает `docker` или `sudo docker` (проверкой `docker info`);
2. собирает образ `v06x-trimui-cross` из `Dockerfile.aarch64`, **только если
   его ещё нет**;
3. запускает внутри контейнера `build-trimui.sh` (configure + make + strip +
   упаковка);
4. возвращает права на `build/` и `release/` вызвавшему пользователю.

Полезные варианты:

```bash
./make.sh rebuild    # принудительно пересобрать кросс-образ (если меняли Dockerfile)
```

Если нужно собрать вручную, без `make.sh` (эквивалент):

```bash
cd platform/trimui
sudo docker build -f Dockerfile.aarch64 -t v06x-trimui-cross .   # один раз
sudo docker run --rm -v "$(cd ../.. && pwd):/builder" \
    -w /builder/platform/trimui v06x-trimui-cross ./build-trimui.sh
sudo chown -R "$USER:$USER" build release
```

---

## 3. Результат сборки

`platform/trimui/release/` — дерево, **зеркалящее корень SD-карты**:

```
release/
├── Emus/VECTOR06C/
│   ├── v06x            # исполняемый файл (stripped, AArch64)
│   ├── launch.sh       # скрипт запуска для CrossMix
│   ├── config.json     # описание эмулятора для лаунчера
│   └── lib/            # собранные рядом Boost .so (без симлинков)
├── Roms/VECTOR06C/     # сюда класть .rom/.fdd/.r0m
├── Icons/Default/Emus/VECTOR06C.png
└── Backgrounds/Default/VECTOR06C.png
```

`release/` и `build/` — генерируемые артефакты, они в `.gitignore` и в
репозитории не хранятся.

---

## 4. Установка на SD-карту

Скопируйте **содержимое** папки `release/` в корень SD-карты
(`/mnt/SDCARD`), чтобы на устройстве получилось:

```
/mnt/SDCARD/Emus/VECTOR06C/{v06x,launch.sh,config.json,lib/}
/mnt/SDCARD/Roms/VECTOR06C/          <- сюда положите свои ROM
/mnt/SDCARD/Icons/Default/Emus/VECTOR06C.png
/mnt/SDCARD/Backgrounds/Default/VECTOR06C.png
```

Положите ROM-файлы (`.rom`, `.fdd`, `.r0m`) в `/mnt/SDCARD/Roms/VECTOR06C/`.
После этого эмулятор появится в меню CrossMix как **VECTOR-06C**.

> **Важно про симлинки.** SD-карта отформатирована в FAT32/exFAT, которые не
> хранят символьные ссылки. `build-trimui.sh` копирует в `lib/` только
> реальные версионированные файлы Boost (`libboost_*.so.N.N.N`), чьё имя
> совпадает с SONAME, — поэтому rpath `$ORIGIN/lib` работает и без симлинков.
> Перед копированием можно проверить: `find release -type l` не должно ничего
> выводить.

---

## 5. Управление

| Кнопка геймпада | Действие в эмуляторе      |
|-----------------|---------------------------|
| D-Pad           | стрелки                   |
| A               | ВК (RETURN)               |
| B               | SPACE                     |
| X               | АП2 (ESC)                 |
| Y               | TAB                       |
| SELECT / BACK   | РУС/ЛАТ (F6)              |
| **START**       | **выход в лаунчер**       |

Сейвы и системное меню TrimUI для standalone-порта недоступны (их предоставляет
только RetroArch); выход из ROM — по START.

---

## 6. Диагностика на устройстве

`launch.sh` пишет лог в `/mnt/SDCARD/Emus/VECTOR06C/v06x.log` — туда попадает
вывод `common_launcher.sh`, `cpufreq.sh` и самого `v06x`, а также код возврата.

Проверить бинарник на Brick Pro:

```bash
cd /mnt/SDCARD/Emus/VECTOR06C
file v06x            # ELF 64-bit LSB ... ARM aarch64
ldd ./v06x           # зависимости должны находиться (Boost из ./lib)
readelf -d ./v06x    # RUNPATH = $ORIGIN/lib
```

Для точечной изоляции подсистем без пересборки рядом с `launch.sh` можно
создать `debug.env`, например:

```sh
export SDL_VIDEODRIVER=kmsdrm     # или dummy
export SDL_AUDIODRIVER=alsa       # или dummy
```

`launch.sh` подхватит этот файл, если он есть.

---

## 7. Файлы порта

| Файл                             | Назначение                                            |
|----------------------------------|-------------------------------------------------------|
| `make.sh`                        | точка входа: одна команда на всю сборку               |
| `Dockerfile.aarch64`             | кросс-образ: SDL2 2.0.12 + Boost 1.75.0 в sysroot     |
| `build-trimui.sh`                | configure + make + strip + упаковка `release/`        |
| `trimui-aarch64.toolchain.cmake` | CMake-toolchain для AArch64                           |
| `package/launch.sh`              | скрипт запуска на устройстве (+ лог)                  |
| `package/config.json`            | описание эмулятора для CrossMix (`extlist: rom\|fdd\|r0m`) |
| `assets/`                        | иконка и фон меню                                     |

Профиль сборки включается флагом CMake `-DTRIMUI=ON` (ветка в корневом
`CMakeLists.txt`): AArch64, `-std=gnu++17`, `-O3`, линковка **без PIE**
(`-no-pie` — нужна из-за встроенных objcopy-blob'ов, чей размер задан
адресом абсолютного символа), Boost линкуется как shared с rpath `$ORIGIN/lib`,
отключены OpenGL, SDL2_image, gperftools, GDB/скриптинг и тесты.
