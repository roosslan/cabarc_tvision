# Cabine

[English](README.md) | **Русский**

CAB-архиватор и файловый менеджер для консоли Windows на
[Turbo Vision](https://github.com/magiblot/tvision). CAB создаётся и изменяется через
системный Cabinet API (`cabinet.dll`); ZIP, RAR, 7Z, GZ, TAR и TAR.GZ — просматриваются
и распаковываются.

![cabine](cabine.gif)

## Возможности

- Одна панель для диска и архивов: директории внутри архива открываются так же, как на
  диске; при запуске открывается место, где программу закрыли.
- Создание, дополнение и удаление файлов в CAB; распаковка, проверка и просмотр (F3)
  архивов всех поддерживаемых форматов. Формат определяется по содержимому файла.
- Степень сжатия CAB из списка — от «Без сжатия» до «Максимальное (LZX:21)».
- Многотомные CAB и самораспаковывающиеся архивы (EXE) — в том числе многотомные;
  распаковщик тоже построен на Turbo Vision.
- Пароли ZIP (PKWARE, WinZip AES) и RAR, включая RAR с зашифрованными заголовками.
- Встроенный просмотр текста с переключением кодировки UTF-8 / Windows-1251 / DOS-866.
- Правка файла по F4 в vim (по умолчанию), Notepad++ или Блокноте; изменённый в vim
  файл из CAB-архива обновляется в архиве.
- Контекстное меню Проводника и пакетный режим: добавление в архив и распаковка
  без панели, с окном хода и кнопкой «Отмена».
- Ассоциация файлов архивов с Cabine и выбор редактора для F4 («Настройки» →
  «Параметры»).
- Русский интерфейс, горячие клавиши с русскими буквами, имена файлов в Юникоде.

## Форматы

| Формат | Чтение | Запись | Библиотека |
|---|---|---|---|
| CAB (в том числе многотомный и SFX) | да | да | Cabinet API Windows |
| ZIP | да | — | minizip-ng, zlib |
| RAR (RAR4, RAR5) | да | — | UnRAR |
| 7Z | да | — | LZMA SDK |
| GZ, TAR, TAR.GZ | да | — | zlib, собственный разбор tar |

## Сборка

Требуется Windows 10 или новее, Visual Studio 2022 и CMake 3.16+. Библиотеки
подключены git-субмодулями.

```bash
git clone --recursive https://github.com/roosslan/cabarc_tvision.git
```

```bash
cmake -S . -B build -A x64
```

```bash
cmake --build build --config Release
```

Результат — `build\Release\cabine.exe` и `cabine-shell.exe` (посредник для контекстного
меню, лежит рядом с `cabine.exe`). Распаковщик самораспаковывающихся архивов встроен в
`cabine.exe`. VC++ Redistributable не требуется.

## Запуск

| Команда | Действие |
|---|---|
| `cabine.exe [<директория \| архив>]` | Панель на указанном месте или там, где программу закрыли |
| `cabine.exe -a <путь>...` | Добавить в `<имя>.cab` рядом с объектами |
| `cabine.exe -ai <путь>...` | То же с окном: имя архива, сжатие, размер тома, SFX |
| `cabine.exe -x <архив>...` | Распаковать в директорию архива |
| `cabine.exe -e <архив>...` | Распаковать в поддиректорию с именем архива |
| `cabine.exe -ei <архив>...` | Распаковать в директорию, выбранную в окне |
| `cabine.exe --register` / `--unregister` | Добавить / убрать подменю «Cabine» в Проводнике и директорию программы в `Path` |

Подменю записывается в `HKCU` текущего пользователя (права администратора не нужны) и
содержит пункты «Открыть в Cabine», «Распаковать здесь», «Извлечь в ...», «Добавить в
CAB-архив» и «Добавить в архив ...». Директория программы добавляется в `Path`
пользователя: в новых окнах консоли `cabine` запускается по имени. После переноса
программы регистрацию необходимо повторить.

## Ограничения

- ZIP, RAR, 7Z, GZ и TAR открываются только для чтения.
- CAB: файл в архиве и том — до 2 Гб, не более 65535 файлов. Изменение выполняется
  пересборкой архива; многотомные архивы изменять нельзя.
- LZX из `cabinet.dll` очень медленный на однотипных данных (например, журналах) —
  для них лучше «Быстрое (MSZIP)».
- Solid-блок 7z распаковывается в память целиком.
- ZIP с методами BZip2, LZMA, Zstandard, XZ, PPMd открывается, но такие файлы не
  распаковываются.
- Файлы для просмотра и запуска извлекаются во временную директорию, которая удаляется
  при выходе из Cabine.
- Настройки хранятся в `%APPDATA%\Cabine\cabine.ini` (директорию можно изменить
  переменной `CABINE_SETTINGS_DIR`).

## Доработки Turbo Vision

Субмодуль `tvision` закреплён на определённом коммите. Сама библиотека не
изменяется — Cabine обходит её ограничения в своём коде:

- строки стандартных окон переведены (`src/tvtext_ru.cpp` вместо `tvtext2.cpp`);
- горячие клавиши с русскими буквами и ввод кириллицы в полях (`src/hotkeys.cpp`);
- собственный диалог выбора файлов вместо `TFileDialog`/`TChDirDialog` (переполнение
  буферов на длинных путях), многострочный текст вместо `TStaticText` (предел 255 байт).

## Сторонние библиотеки

- **Turbo Vision** — исходный код Borland, опубликованный Borland; изменения и новый
  код magiblot — лицензия MIT.
- **zlib**, **minizip-ng** — лицензия zlib.
- **LZMA SDK** (из репозитория 7-Zip) — общественное достояние, (c) Igor Pavlov.
- **UnRAR** — (c) Alexander Roshal, используется без изменений только для распаковки.
  Условие лицензии UnRAR (полный текст — `third_party/unrar/license.txt`):

  > UnRAR source code may be used in any software to handle RAR archives without
  > limitations free of charge, but cannot be used to develop RAR (WinRAR) compatible
  > archiver and to re-create RAR compression algorithm, which is proprietary.
  > Distribution of modified UnRAR source code in separate form or as a part of other
  > software is permitted, provided that full text of this paragraph, starting from
  > "UnRAR source code" words, is included in license, or in documentation if license
  > is not available, and in source code comments of resulting package.
