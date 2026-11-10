# Тест-план: расписание тем (Appearance → Schedule)

Проходить **целиком перед каждой отправкой кода проверяющим** (push в MR, новый релиз PPA).
Разделы A–F относятся к коду MR. Раздел H — только для сборок с новыми функциями (sunrise/sunset,
цветовая температура, Location); для MR-сборки проверить, что этих функций там **нет**.

Отметки: ✅ работает, ❌ ошибка (записать, что именно), — неприменимо.

---

## 0. Подготовка

**Стенд.** Виртуальная машина Xubuntu 26.04 (virt-manager или GNOME Boxes):
- снимок «чистая система» до установки пакета — после каждого прогона откатываться на него;
- в ВМ можно свободно переводить часы, это не трогает основную систему;
- в ВМ нет подсветки экрана → яркость (B11) проверять на ноутбуке, в ВМ проверять только
  поведение «подсветки нет» (A11).

**Темы для проверки** (есть в Xubuntu + `sudo apt install materia-gtk-theme numix-gtk-theme
elementary-xfce-icon-theme yaru-theme-icon dmz-cursor-theme`; если какого-то пакета нет — взять
аналог):

| Назначение | GTK-тема | Иконки | Курсор |
|---|---|---|---|
| есть пара светлая/тёмная | Greybird / Greybird-dark | elementary-xfce / elementary-xfce-dark | |
| тёмный вариант в середине имени | Orchis-Compact / Orchis-Dark-Compact, Materia-compact / Materia-dark-compact | Yaru / Yaru-dark | |
| нет тёмного варианта | Numix, HighContrast | Tango | |
| без темы xfwm4 | Adwaita / Adwaita-dark | | |
| курсоры | | | DMZ-White, DMZ-Black, Adwaita |

**Управление временем в ВМ:**
```
sudo timedatectl set-ntp false
sudo timedatectl set-time 19:59:30      # правило на 20:00 сработает через ~30 с
sudo timedatectl set-ntp true           # вернуть в конце
```
Без перевода часов: ставить правила на «сейчас + 2 минуты».

**Наблюдение** (держать открытым второй терминал):
```
watch -n1 'xfconf-query -c xsettings -p /Net/ThemeName;
           xfconf-query -c xsettings -p /Net/IconThemeName;
           xfconf-query -c xsettings -p /Gtk/CursorThemeName;
           xfconf-query -c xfwm4 -p /general/theme;
           gsettings get org.gnome.desktop.interface color-scheme;
           cat /sys/class/backlight/*/brightness 2>/dev/null'
```
Демон с выводом в терминал: `XFSETTINGSD_DEBUG=theme-schedule xfsettingsd --replace`
(отладочные сообщения могут быть выключены в release-сборке, предупреждения видны всегда).
Диалог тоже запускать из терминала (`xfce4-appearance-settings`) и следить, чтобы не было
`CRITICAL` / `WARNING`.

Внешняя проверка цветовой схемы: GTK4/libadwaita-приложение (например, `gnome-text-editor` или
`gnome-calculator`), Firefox (Website appearance = Automatic), Chromium (Mode = Device).

---

## A. Диалог: вкладка Schedule

| # | Действие | Ожидается |
|---|---|---|
| A1 | Открыть вкладку при пустом расписании и выключенном переключателе | Список и кнопки неактивны; строка статуса пустая или поясняет, что выключено |
| A2 | Включить переключатель | Список активен; `xfconf-query -c xsettings -p /Xfce/ThemeSchedule/Enabled` = true; тема **не** меняется сразу |
| A3 | Add | Диалог «Add Rule»; время — ближайший свободный полный час; Theme = Dark variant, Color scheme = Dark; фокус в поле часов |
| A4 | Add → Cancel / Escape / закрыть окно | Ничего не добавлено |
| A5 | Add → OK | Строка появилась на месте по времени (список отсортирован); жирное время + под ним **название темы** |
| A6 | Строка с «Dark variant» при текущей теме Greybird | Под временем «Greybird-dark» |
| A7 | Строка с Theme = No change | Под временем то, что правило всё же меняет (см. A27); «No change» — только если правило не меняет ничего |
| A8 | Строка с «Dark variant», когда у темы до правила нет тёмного варианта (Numix) | Под временем «no dark variant of Numix»; в подсказке «Theme: Numix stays, it has no dark variant»; в строке статуса (если правило следующее) «no dark variant of Numix» |
| A9 | Сменить тему на вкладке Style (Greybird → Orchis-Compact) | Строки с вариантами сразу показывают Orchis-Dark-Compact / Orchis-Compact |
| A10 | Открыть правило: двойной клик / Enter / кнопка Edit | Диалог «Edit Rule» со всеми сохранёнными значениями |
| A11 | Яркость без подсветки (ВМ) | Флажок неактивен, подсказка «No screen backlight was found.»; сохранённое значение не теряется |
| A12 | Изменить время правила на занятое | Красное сообщение «There is already a rule at HH:MM.», сигнал, диалог остаётся открытым; после исправления OK сохраняет |
| A13 | Изменить время правила на свободное | Строка переезжает на новое место, выделение на ней |
| A14 | Remove (одно правило; несколько, если список позволяет выделить несколько) | Удалены, сохранено в xfconf |
| A15 | Списки Theme, Icon theme, Cursor theme | Открываются **под полем**, без пустого места сверху; длинный список прокручивается; выбор с клавиатуры (стрелки, буквы) работает |
| A16 | Выбрать в Theme тёмную тему / Dark variant / светлую / Light variant | Color scheme сам переключается на Dark / Dark / Light / Light; после этого его можно поменять вручную |
| A17 | Флажки icon / cursor / brightness | Включают и выключают поле рядом; правило с выключенным флажком при повторном открытии показывает его выключенным, в xfconf пустая строка |
| A18 | Подсказки | Есть у: Time, Theme, Color scheme, Window manager theme, Icon theme, Cursor theme, Brightness, Run command |
| A19 | Enter в полях часов, минут, яркости, команды | Срабатывает OK |
| A20 | Строка статуса | «Next switch at …» правильно указывает следующее правило (после последнего за сутки — первое правило завтра); обновляется при включении/выключении, смене темы, раз в 30 с |
| A21 | Закрыть и открыть диалог; `xfconf-query -c xsettings -l | grep ThemeSchedule` | Всё сохранено; массивы одинаковой длины |
| A22 | Вкладка Style | Под списком кнопки Add и ⓘ (из !224), кнопки «?» нет |
| A23 | Вкладка Style: кнопка Add (установка темы) | Работает как раньше, не сдвинута |
| A24 | Только клавиатура: Tab по вкладке и диалогу, мнемоники (Alt+T, Alt+H, Alt+R, Alt+W, Alt+I, Alt+U, Alt+B, Alt+M; Alt+C = Cancel, Alt+O = OK — все разные) | Все элементы достижимы |
| A25 | Диалог в тёмной теме; масштаб 2× (Settings → Window Scaling); узкое окно | Всё читается, ничего не обрезано |
| A26 | Только PPA-сборки с переводами (в MR переводов нет, их делают через Transifex): `LANG=ru_RU.UTF-8 xfce4-appearance-settings`, то же для de | Переведённые строки на месте, ничего не обрезано |
| A27 | *(schedule-summary)* Правило только с яркостью 75 % | Под временем «brightness 75%» |
| A28 | *(schedule-summary)* Правило: Dark variant + Dark + значки elementary-xfce-dark + курсор DMZ-White + яркость 60 + команда | Под временем «Greybird-dark, brightness 60%, icons: elementary-xfce-dark, cursor: DMZ-White, command»; в узком окне обрезано многоточием |
| A29 | *(schedule-summary)* Наведение мыши на каждое правило | Подсказка: по строке на настройку (Theme … (dark variant), Window manager theme, Color scheme, Screen brightness, Icon theme, Mouse cursor theme, Command с текстом команды); у правила без изменений подсказки нет |
| A30 | *(schedule-summary)* Цветовая схема: тёмная тема + Dark; тёмная тема + Light; без темы + Dark; без темы + Default | Схема не показана / «light color scheme» / «dark color scheme» / «default color scheme» |
| A31 | *(schedule-summary)* Значки = Dark variant; сменить тему значков на вкладке Icons | «icons: <тёмный вариант текущих значков>», обновляется сразу после смены |
| A32 | *(schedule-summary)* Строка статуса, когда следующее правило не меняет тему | «Next switch at HH:MM: brightness 75%» (тот же текст, что в списке), а не «N more actions» |
| A33 | *(schedule-summary)* Правило 18:00 Numix + значки Tango, затем открыть правило 19:30 с Dark variant и значками Dark variant | Под Theme и Icon theme предупреждения со значком: «Numix has no dark variant, so the theme will stay.», «Tango has no dark variant, so the icon theme will stay.» |
| A34 | *(schedule-summary)* В том же диалоге сменить час на 17 (до правила 18:00) | Предупреждение о теме меняется/пропадает по теме, действующей к 17:00 (с учётом правил через полночь); при выборе конкретной темы или No change предупреждения нет |
| A35 | *(schedule-summary)* Цепочка: 22:00 Adwaita-dark, 07:00 Light variant, 13:00 Dark variant | Список: 07:00 «Adwaita», 13:00 «Adwaita-dark» (варианты считаются от темы предыдущих правил, а не от текущей) |
| A36 | *(schedule-summary)* Изменить/удалить правило 18:00 | Подписи и подсказки остальных правил сразу пересчитываются |

---

## B. Демон: каждая функция отдельно

Для каждой строки: одно правило на «сейчас + 1–2 минуты», остальные настройки правила выключены
или No change. Перед каждой проверкой выставить исходное состояние руками.

| # | Исходное | Правило | Ожидается |
|---|---|---|---|
| B1 | Greybird | Theme = Adwaita (другая тема, не вариант) | ThemeName = Adwaita, окна перерисовались |
| B2 | Greybird | Dark variant | Greybird-dark |
| B3 | Orchis-Compact; Materia-compact | Dark variant | Orchis-Dark-Compact; Materia-dark-compact |
| B4 | Greybird-dark | Light variant | Greybird |
| B5 | Numix | Dark variant | Тема не меняется (в отладочном выводе «has no dark variant»), остальные настройки правила применяются |
| B6 | Greybird-dark | Dark variant | Ничего не меняется |
| B7 | любая | Theme в xfconf = несуществующая (`xfconf-query … -p /Xfce/ThemeSchedule/Themes` вручную) | Предупреждение «not installed», тема не меняется, демон работает дальше |
| B8 | color-scheme = default | Theme = No change, Color scheme = Dark / Light / Default / No change | prefer-dark / prefer-light / сброс в default / без изменений; GTK4-приложение и Firefox переключаются |
| B9 | Greybird, xfwm4 = Greybird | Dark variant, флажок window manager **включён** | xfwm4 = Greybird-dark |
| B10 | то же | Dark variant, флажок **выключен** | xfwm4 остаётся Greybird |
| B11 | Greybird | Adwaita-dark, флажок включён | xfwm4 = Default (у Adwaita нет темы xfwm4) |
| B12 | старое правило, Xfwm4 = "" в xfconf | Dark variant | xfwm4 следует переключателю «Set matching Xfwm4 theme» на вкладке Style |
| B13 | elementary-xfce | Icon theme = Yaru (по имени) | IconThemeName = Yaru |
| B14 | elementary-xfce; Yaru | Icon theme = Dark variant | elementary-xfce-dark; Yaru-dark |
| B15 | Tango | Icon theme = Dark variant | Не меняется |
| B16 | DMZ-White | Cursor = DMZ-Black | CursorThemeName = DMZ-Black, курсор сменился (в новых окнах сразу) |
| B17 | (ноутбук) 100 % | Brightness = 40 | Подсветка 40 %, без запроса пароля |
| B18 | — | Command = `notify-send "$XFCE_SCHEDULE_GTK_THEME / $XFCE_SCHEDULE_ICON_THEME / $XFCE_SCHEDULE_COLOR_SCHEME"` | Уведомление с новыми значениями |
| B19 | — | Command с конвейером: `date >> ~/sched.log && echo ok >> ~/sched.log` | Работает (выполняется через /bin/sh) |
| B20 | — | Command = `несуществующая-команда` | /bin/sh пишет «not found» в вывод демона, тема и остальное применяются |
| B21 | — | Только Command (тема и схема No change) | Команда выполняется |
| B22 | — | Правило, которое ничего не делает (всё No change / выключено) | Игнорируется, ничего не происходит |

---

## C. Демон: время срабатывания

| # | Сценарий | Ожидается |
|---|---|---|
| C1 | Правило на ЧЧ:ММ | Срабатывает в течение первой минуты после ЧЧ:ММ:00 |
| C2 | После срабатывания сменить тему руками | Ручная тема держится до следующего правила |
| C3 | Изменить/добавить правило со временем, которое сегодня уже прошло | Сразу **ничего не переключается** (демон перечитывает правила через 250 мс после последней записи) |
| C4 | Выключить и включить переключатель расписания | Сразу ничего не переключается; дальше срабатывает по времени |
| C5 | Выключить расписание, пройти через время правила | Не срабатывает |
| C6 | Выйти из сеанса до 20:00, перевести часы на 20:30, войти | Правило 20:00 применяется при входе |
| C7 | То же, но пропущены два правила (20:00: тема + иконки + команда, 22:30: только тема + команда), вход в 23:00 | Одно переключение: тема от 22:30, иконки от 20:00; выполняется только команда 22:30 |
| C8 | Сон (suspend) через время правила — `_tmp/c8-suspend-test.sh [-b 40]` на ноутбуке (с `-b` заодно B17) | После пробуждения правило применяется в течение минуты, пропущенные объединяются (как C7) |
| C9 | Часы назад (с 20:30 на 19:00) | Ничего не срабатывает повторно; в 20:00 срабатывает снова |
| C10 | Часы вперёд через несколько правил | Одно переключение на объединение пройденных правил (как C7) |
| C11 | Переход через полночь: правила 23:59 и 00:01 | Оба срабатывают в свой день |
| C12 | Смена часового пояса (`sudo timedatectl set-timezone Asia/Tokyo`) | Правила по местному времени, без шквала переключений |
| C13 | `xfsettingsd --replace` между правилами | Расписание продолжает работать, без повторного срабатывания |

---

## D. Сочетания

Все сочетания перебрать нельзя (4 × 4 × 2 × 3 × 2 × 2 × 2 = 768). Набор ниже покрывает каждое
значение каждой настройки и большинство пар между ними. Правила ставить подряд с интервалом
2 минуты (или переводить часы) и после каждого сверять **всё** состояние из окна наблюдения.

| # | Theme | Color scheme | WM | Icons | Cursor | Brightness* | Command |
|---|---|---|---|---|---|---|---|
| D1 | Dark variant | Dark | вкл | Dark variant | DMZ-Black | 40 | notify-send |
| D2 | Light variant | Light | вкл | Light variant | DMZ-White | 90 | — |
| D3 | Adwaita-dark (имя) | Default | выкл | — | — | 60 | notify-send |
| D4 | No change | Dark | выкл | Yaru-dark (имя) | — | — | — |
| D5 | Orchis-Compact (имя) | No change | вкл | — | Adwaita | — | notify-send |
| D6 | No change | No change | выкл | — | — | 30 | — |
| D7 | Dark variant (исходная Numix — варианта нет) | Dark | вкл | Dark variant (Tango — варианта нет) | — | — | notify-send |
| D8 | Greybird (имя) | Light | вкл | elementary-xfce (имя) | DMZ-White | 100 | — |

\* Яркость — только на ноутбуке; в ВМ эти правила проверяют, что без подсветки остальное
применяется нормально.

**Сценарии пользователя:**
- День/ночь: 07:00 Light variant + Light, 20:00 Dark variant + Dark. Прожить сутки (переводом часов),
  включая выход из сеанса на ночь.
- Три фазы: 07:00 Light variant, 19:00 Dark variant + яркость 60, 23:00 Adwaita-dark + яркость 30 +
  курсор DMZ-Black.
- Только яркость, без смены тем: 08:00 яркость 100, 21:00 яркость 40.

---

## E. Совместимость и пакет

| # | Проверка | Ожидается |
|---|---|---|
| E1 | Правила от старой версии без массива ColorSchemes | Загружаются; цветовая схема следует теме (тёмная тема → Dark) |
| E2 | Время в xfconf не в формате ЧЧ:ММ (например, `sunrise` от сборки с новыми функциями) | Демон пропускает правило; диалог не падает |
| E3 | Массивы разной длины (вручную через xfconf-query) | Недостающие значения считаются пустыми, ничего не падает |
| E4 | `sudo apt install ./xfce4-settings_….deb ./xfce4-helpers_….deb`, затем `xfsettingsd --replace` | Ставится поверх штатного пакета, расписание работает |
| E5 | Откат на штатный пакет (`apt install --allow-downgrades xfce4-settings=<версия из archive>`) | Система работает, ключи ThemeSchedule в xfconf просто игнорируются |

---

## F. Регрессия остального

- Вкладки Style, Icons, Fonts, Settings: выбор темы, иконок, шрифтов, сглаживания — как в штатной версии.
- Переключатель «Set matching Xfwm4 theme» на вкладке Style работает.
- xfsettingsd: раскладка клавиатуры, мышь, дисплеи, горячие клавиши — работают после `--replace`.
- В выводе диалога и демона нет `CRITICAL` / `WARNING`, кроме ожидаемых в B7 и B20.

---

## G. Перед отправкой (сборка)

- master: `meson compile` без предупреждений; `git clang-format` (clang-format 19 из `_tmp/cf/bin`) без изменений.
- Сравнить diff с тем, что уже в MR: новым может быть только то, что просили проверяющие.
- Собрать тестовый .deb для 4.20 (только код MR) и пройти разделы A–F на нём.
- Свежие скриншоты: список, диалог правила.

---

## H. Только сборки с новыми функциями (не MR)

Для MR-сборки: в диалоге **нет** Sunrise/Sunset, цветовой температуры, Location;
`strings /usr/bin/xfce4-appearance-settings | grep -i -e sunrise -e temperature -e location` пусто.

Для сборок v0.5+ (заполнить подробно при переносе редизайна в ветки функций):
- правила «sunrise» / «sunset» ± смещение; Location (ручной ввод, поиск города); смена дат и
  полярный день/ночь (время восхода не существует);
- цветовая температура: плавный переход, сохранение при выходе, несколько мониторов;
- сочетания: правило по восходу + смена темы + яркость + температура.
