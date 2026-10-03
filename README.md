# NAX5

Клиент NAX5 для Windows: удалённая игра на физической PlayStation 5 из облака NAX5. Сайт сервиса: [cloudgta6.com](https://www.cloudgta6.com/).

## Скачать

Актуальная версия: **Alpha 0.5 build 15** для Windows x64.

[Скачать NAX5-windows.zip](https://github.com/NAX5-Cloud/NAX5/releases/download/alpha-0.5-build-15/NAX5-windows.zip) (76 МБ)

Все версии и заметки к выпускам: [Releases](https://github.com/NAX5-Cloud/NAX5/releases).

## Требования

- Windows 10 или 11 (x64);
- геймпад по USB или Bluetooth;
- стабильный интернет: ориентир 15–20 Мбит/с на поток Full HD, лучше по кабелю или Wi-Fi 5 ГГц;
- аккаунт на [сайте NAX5](https://www.cloudgta6.com/): регистрация и подтверждение email.

## Как начать

1. Зарегистрируйтесь на сайте и подтвердите email.
2. Распакуйте архив и запустите `NAX5.exe`.
3. Войдите с тем же email и паролем, затем нажмите «Играть».

Не нужно добавлять PS5 вручную, указывать IP, PIN или токен PSN.

## Windows SmartScreen и контрольная сумма

Сборка пока без цифровой подписи, поэтому Windows SmartScreen может показать предупреждение. Убедиться, что файл не изменён, можно по контрольной сумме.

SHA-256 файла `NAX5-windows.zip` (build 15):

```
ddc1cb39386a6ee4c2ea96e614b1034737df390e9f2b437848e359ef501dcf79
```

Проверка в PowerShell:

```
Get-FileHash NAX5-windows.zip -Algorithm SHA256
```

Контрольные суммы других сборок GitHub показывает рядом с файлами в разделе [Releases](https://github.com/NAX5-Cloud/NAX5/releases).

## Поддержка

- Чат поддержки в Telegram: <https://t.me/+SIDn9T3yD5kyMjhi>
- Сообщить об ошибке: [Issues](https://github.com/NAX5-Cloud/NAX5/issues)
- Известные проблемы: [KNOWN-ISSUES.md](KNOWN-ISSUES.md)

## Лицензия и исходный код

NAX5 основан на [chiaki-ng](https://github.com/streetpea/chiaki-ng) v1.10.0 и распространяется под лицензией AGPL-3.0. Условия и сторонние лицензии: [COPYING](COPYING), [LICENSES](LICENSES), [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Исходный код каждой выпущенной сборки находится в теге её релиза, например `alpha-0.5-build-15`.

Разработчикам: [NAX5.md](NAX5.md), [FORK-MAINTENANCE.md](FORK-MAINTENANCE.md), [UPSTREAM.md](UPSTREAM.md).
