# m3eGram

**Telegram Desktop в стиле Material Design 3 Expressive.**
Неофициальный форк [Telegram Desktop](https://github.com/telegramdesktop/tdesktop): весь интерфейс переработан под M3E, сборки делаются в облаке (GitHub Actions).

> Это экспериментальный сторонний клиент. Он не связан с Telegram FZ-LLC и не одобрен ею.
> Для работы нужны **собственные** `api_id` и `api_hash` (бесплатно: <https://my.telegram.org> → *API development tools*).

## Что изменено

**Форма и компоненты**
- Список чатов: активный и наведённый чат — скруглённая «пилюля», вкладки папок — тонированные пилюли.
- Поле ввода сообщения — полная пилюля; единая шкала скруглений M3E (8 / 12 / 16 / 20 / 28) в боксах, настройках, профиле, звонках, редакторе, платежах и т. д.
- Переключатели 52×32 в стиле M3, пилюли в меню и в списках настроек, скруглённые меню, тосты и подсказки.
- Всплывающее уведомление о сообщении — скруглённая карточка.
- Исправлены места, где радиус больше половины элемента (кнопки бота, плашки времени, вкладки тем, верхние панели): раньше там появлялись полосы или «дырки».

**Блюр и «живой фон»**
- Под боксами, настройками и главным меню вместо чёрного затемнения — размытый снимок окна.
- Превью стикеров и GIF — на размытом фоне.
- Переключатель **«Живой фон (размытие в реальном времени)»** (Настройки, под ползунком масштаба интерфейса): просмотр медиа становится полупрозрачным с настоящим размытием того, что за окном. Сейчас только Windows.
- В просмотре медиа ссылки-кнопки стали читаемыми на тёмном фоне.

**Иконки**
- 579 иконок интерфейса заменены на [Material Symbols](https://github.com/google/material-design-icons) (Rounded), 1737 PNG (1x/2x/3x) с теми же размерами, что у оригиналов.
- Новая иконка приложения (панель задач, окно, `.exe`).

## Сборки

Готовые файлы собираются в GitHub Actions и лежат в артефактах запуска:

| Платформа | Workflow | Запуск | Артефакт |
|---|---|---|---|
| Windows x64 | `build-windows.yml` | вручную (*Run workflow*) | `tdesktop-m3e-expressive-win64` |
| Linux x64 | `build-linux.yml` | автоматически при пуше в `main` | `tdesktop-m3e-expressive-linux` |

Windows-сборка долгая: она собирает Qt, FFmpeg, OpenSSL и т. д. с нуля (порядка 5 часов).

**Перед первой сборкой** добавьте в *Settings → Secrets and variables → Actions* два секрета: `TDESKTOP_API_ID` и `TDESKTOP_API_HASH`.

## Как устроен репозиторий

- `Telegram/SourceFiles` — исходники клиента, правки внесены прямо в них.
- `Telegram/lib_ui` — сабмодуль. Его стили и виджеты правятся патчем **`patches/lib-ui-md3.patch`**, который CI накладывает на закреплённый коммит сабмодуля.
- `tools/m3e_icons` — генератор иконок и таблица соответствий `mapping.json` (иконка Telegram → глиф Material Symbols); см. [`tools/m3e_icons/README.md`](tools/m3e_icons/README.md).
- `patches/README.md` — подробный список изменений по файлам.

### Пересборка патча `lib_ui`

```powershell
git clone https://github.com/desktop-app/lib_ui.git lib_ui-work
cd lib_ui-work
git checkout <закреплённый коммит из Telegram/lib_ui>
git apply ..\patches\lib-ui-md3.patch
# ...правки...
git add -A
git diff --cached <закреплённый коммит> --output=..\patches\lib-ui-md3.patch
git apply --check ..\patches\lib-ui-md3.patch   # на чистом checkout
```

### Перегенерация иконок

```powershell
pip install pillow
cd tools\m3e_icons
python generate.py --apply        # иконки интерфейса
python make_app_icon.py --apply   # иконка приложения
python generate.py --restore      # вернуть оригиналы
```

## Лицензия

Исходный код распространяется под GPLv3 с исключением для OpenSSL, как и у Telegram Desktop: см. [LICENSE](LICENSE).
Оригинальный README Telegram Desktop сохранён в [`docs/README.upstream.md`](docs/README.upstream.md) (там же список сторонних компонентов).
Инструкции по сборке на своей машине: [Windows](docs/building-win.md), [macOS](docs/building-mac.md), [Linux](docs/building-linux.md).

---

### English summary

m3eGram is an unofficial Telegram Desktop fork redesigned in Material Design 3 Expressive (pill-shaped list items and tabs, M3 switches and menus, frosted scrims, Material Symbols icons, a new app icon and a "live background" toggle for real-time blur on Windows). Builds are produced by GitHub Actions; add your own `TDESKTOP_API_ID` / `TDESKTOP_API_HASH` secrets first. Not affiliated with Telegram.
