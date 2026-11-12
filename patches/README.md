# m3eGram — tdesktop × Material Design 3 Expressive

Приватный снапшот [telegramdesktop/tdesktop](https://github.com/telegramdesktop/tdesktop) (dev)
с патчем **MD3 Expressive**: единые 20dp-скругления у всех сообщений-пузырей
(20dp — «bold»-шаг шкалы форм M3 Expressive, старое значение 16dp).

## Что здесь

- Исходники tdesktop @ dev + применённый `patches/m3e-expressive.patch` (см. `Telegram/SourceFiles/ui/chat/chat.style`)
- `.github/workflows/build-linux.yml` — сборка Linux-бинарника официальным Docker-окружением
- `.github/workflows/build-windows.yml` — сборка Windows x64 (VS2026 на `windows-latest`), экспериментальная

## Как собрать

1. Получи **api_id** и **api_hash** (бесплатно, 2 минуты): https://my.telegram.org → *API development tools*.
2. Settings → Secrets and variables → Actions → New repository secret:
   - `TDESKTOP_API_ID` = твой api_id
   - `TDESKTOP_API_HASH` = твой api_hash
3. Вкладка **Actions** → выбери workflow → **Run workflow**:
   - *Build tdesktop Linux* — надёжный вариант (~3–5 часов), артефакт `tdesktop-m3e-expressive-linux`
   - *Build tdesktop Windows* — эксперимент (~2–4 часа), артефакт `tdesktop-m3e-expressive-win64`

Артефакт появится внизу страницы запуска (Build → Artifacts).

## Зачем api_id

Без `TDESKTOP_API_ID`/`TDESKTOP_API_HASH` клиент не пройдёт логин на серверах Telegram —
это требование MTProto, а не CI. Креды хранятся только в секретах твоего приватного репо.

## Обновление патча

Снапшот — одиночный коммит поверх upstream dev. Хочешь ещё «экспрессива» (больше радиусов,
шейпы) — правь `.style`/`.cpp` файлы в ветке и просто перезапускай workflow.
