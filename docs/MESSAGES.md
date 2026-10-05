# Launcher messages and error codes

What the player sees, the server answer behind it, and the number that appears in the log as
`reserve failed: N` or `heartbeat failed: N` (`Nax5SessionError`, `gui/include/nax5/session/nax5sessionerror.h`).
New codes are added at the end only: the numbers are used in log analysis.

| N | Name | Server answer | Text on screen |
| --- | --- | --- | --- |
| 1 | NoCapacity | 409 `NO_CAPACITY` | Консоль сейчас занята. Лаунчер сообщит здесь, когда она освободится. |
| 2 | UserNotEligible | 403 `USER_NOT_ELIGIBLE` | Этот аккаунт пока не может получить консоль. |
| 3 | ActiveSessionExists | 409 `ACTIVE_SESSION_EXISTS` | У вас уже есть выделенная консоль. |
| 4 | Unauthenticated | 401 | Сессия входа истекла. Войдите снова. |
| 5 | NetworkError | no answer | Нет подключения к интернету. |
| 6 | RateLimited | 429 | Слишком много попыток. Попробуйте позже. |
| 7 | ServerError | 5xx | Сервис временно недоступен. |
| 8 | InvalidResponse | unreadable answer | Сервис временно недоступен. |
| 9 | Forbidden | 403 `SESSION_NOT_OWNED` | Нельзя освободить чужую сессию. |
| 10 | NotFound | 404 `SESSION_NOT_FOUND` | Игровая сессия не найдена. |
| 11 to 15 | connection material, host and timeout errors | various | Не удалось подключиться к консоли. |
| 16 | ClientUpdateRequired | 426 `CLIENT_UPDATE_REQUIRED` | Доступно обязательное обновление NAX5… |
| 17 | InsufficientBalance (build 16) | 403 `INSUFFICIENT_BALANCE` | Игровое время закончилось. Пополните его в аккаунте на сайте и нажмите «Играть». |
| 18 | SessionCooldown (build 16) | 429 with `reason: SESSION_COOLDOWN` | Консоль ждут другие игроки. Вы играли больше 3 часов, попробуйте снова через 10 минут. |
| 19 | ConsoleOffline (build 16) | 409 `CONSOLE_OFFLINE` | Консоль сейчас выключена. Мы уже знаем об этом и включим её. Попробуйте позже. |

Notes:

- An older launcher maps an unknown 403 to code 2 and an unknown 429 to code 6, so builds before 16 show
  «аккаунт пока не может получить консоль» when there is no play time.
- Code 10 on a heartbeat is not an error: it means the server closed the session, and the launcher stops the
  stream.
- Code 6 right after code 1 is the server's 60 second pause after «no console», not a fault. From build 16 the
  button is blocked for that minute with «Повторить можно через N с», so code 6 should become rare.

## Status line

| Text | When |
| --- | --- |
| Ищем свободную консоль... | reserve in flight |
| Подключаемся... | connection material received, stream starting |
| Запуск PS5... | wake-up sent, waiting for the console |
| Игра | first frame decoded |
| Освобождаем консоль... | ending or cancelling |
| Сессия завершена сервером | heartbeat 404 without a reason |
| Консоль выключили или перевели в режим покоя. Пожалуйста, не выключайте её: после игры просто закройте лаунчер. | the console ended the stream itself, quit reason 12 (build 16) |
| После сбоя видеодрайвера декодер переключён на Direct3D. Нажмите «Играть». | first login after a crash in a Vulkan driver (build 16) |
| Игровое время закончилось | heartbeat 404, `BALANCE_EXHAUSTED` (build 16) |
| Сессия завершена: вы играли больше 3 часов, а консоль ждут другие игроки | heartbeat 404, `SESSION_LIMIT_REACHED` (build 16) |

## Notices over the stream (build 16)

| Text | When |
| --- | --- |
| Осталось 5 минут игрового времени. | time left drops to 300 s |
| Осталась 1 минута игрового времени. Сохранитесь: игра остановится. | time left drops to 60 s |

A session that starts with less than five minutes shows the notice at its first heartbeat.
