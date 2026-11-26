# План: cockpit-gluster-block

Ветка `cockpit-plugin` (от master 0.5.33). Модуль Cockpit для управления
gluster-block, отдельный RPM-пакет `cockpit-gluster-block`.

Статус: апрув получен 2026-11-26. Готово: этапы 0 и 1 (§8, отметки «готово»).

## 0. Термины

| В задаче | В gluster-block | Команда CLI |
|---|---|---|
| volume (создать/удалить/ресайз) | блок `<volname>/<blockname>` | `create`, `delete`, `modify ... size` |
| volume (статус хостов, maintenance) | block hosting volume `<volname>` | `hosts <volname>`, `hosts ... maintenance` |
| вес path'а | ALUA-группа LUN на хосте: `ao` (prio path) / `ano` | `modify ... prio-path`, `hosts ... rebalance` |

Сейчас «вес» бинарный: у каждого блока ровно один prio-хост (AO), остальные
ANO. Числового веса нет и в этой ветке не будет (В3). Идея веса хоста для
rebalance записана в BACKLOG.md.

## 1. Архитектура

```
Браузер ── cockpit-ws ── cockpit-bridge ─┐ cockpit.http({unix: ...})
                                          ▼
k8s оператор / CSI ──HTTPS (mTLS/токен)──> gluster-block-apid ── unix socket (RPC GLUSTER_BLOCK_CLI) ──> gluster-blockd
curl, скрипты ───────────────────────────┘   (каждый узел)                                                │ RPC 24010
gluster-block CLI ──────────── unix socket (как сейчас) ─────────────────────────────────────────────> │
                                                                                         gluster-blockd других хостов
```

- Между клиентами и демоном стоит новый сервис **`gluster-block-apid`**
  (§1a), это HTTP-мост к RPC. Cockpit-плагин работает только через него, без
  `cockpit.spawn` и shell.
- Плагин — только UI. Вся логика (проверки, async, rollback) остаётся в
  gluster-blockd.
- Плагин открывается на **любом** узле кластера и управляет всеми хостами
  томов через локальный apid → локальный демон. Мультихостовый Cockpit (ssh)
  не нужен.
- Список block hosting volumes: `GET /api/v1/volumes` (§1a).
- Автообновление: 10 с (настраивается), пауза при скрытой вкладке.

## 1a. gluster-block-apid: HTTP API

### Почему отдельный сервис, а не HTTP внутри gluster-blockd

- Демон остаётся маленьким: TLS, аутентификация и HTTP-парсер не попадают в
  процесс, который правит LIO и метаданные. Падение или рестарт apid не
  трогает демон (а демон и так не в I/O-пути).
- apid можно не ставить или не открывать наружу: по умолчанию слушает только
  unix socket.
- Демон уже отдаёт готовый JSON (`json_resp`), а CLI — тонкий клиент
  RPC `GLUSTER_BLOCK_CLI` через `clntunix_create`. apid делает то же самое:
  переиспользует rpcgen-стабы (`block_clnt.c`) и общий код CLI вынесем в
  `libgbclient` (внутренняя статическая библиотека). Новый RPC не нужен.

### Реализация

- C, **libmicrohttpd** (есть в EL8/EL9 и Debian/Ubuntu) + json-c + GnuTLS
  (через libmicrohttpd). Пул потоков, таймаут запроса к демону
  (`GB_CLI_TIMEOUT`).
  Go или Python отпадают: для Go нет нормального ONC RPC/XDR, а Python
  вернул бы нас к вызову CLI через shell.
- Слушает:
  - `unix:/run/gluster-block/api.sock` (по умолчанию, `0600 root:root`, В9)
    — для Cockpit (bridge с superuser) и локальных скриптов root, без
    дополнительной аутентификации;
  - `tcp` (выключено по умолчанию) — только TLS; аутентификация (В11)
    по клиентскому сертификату (mTLS, для оператора) **и** по bearer-токену
    (для скриптов) из файла `/etc/gluster-block/api-tokens` (`0600`, хэши,
    роль `admin`/`read`). Для mTLS роль задаёт сопоставление CN → роль в том
    же файле.
- Настройки в `/etc/sysconfig/gluster-block-apid` (по образцу gluster-blockd):
  `GB_API_LISTEN`, `GB_API_TLS_CERT/KEY/CA`, `GB_API_TOKENS_FILE`.
- systemd `gluster-block-apid.service` (`After/Wants=gluster-blockd`), может
  стартовать через socket activation. Логи в `/var/log/gluster-block/apid.log`,
  аудит: кто (CN сертификата / имя токена) и какую мутирующую операцию вызвал.
- Подпакет `gluster-block-api` из **этого же** дерева (autotools, тот же spec),
  ему нужны rpc-стабы. Cockpit-плагин требует `gluster-block-api`.

### API v1 (OpenAPI в `docs/api/openapi.yaml`)

| Метод и путь | Команда |
|---|---|
| `GET /api/v1/volumes` | список gluster-томов + признак block hosting |
| `GET /api/v1/volumes/{vol}/blocks` | `list` (+ `info` с `?detail=1`) |
| `POST /api/v1/volumes/{vol}/blocks` | `create` → `202` + operation |
| `GET /api/v1/volumes/{vol}/blocks/{blk}` | `info` |
| `PATCH /api/v1/volumes/{vol}/blocks/{blk}` | `modify` size → `202` / auth / `prio_path` |
| `DELETE /api/v1/volumes/{vol}/blocks/{blk}` | `delete` (`?unlink_storage=`, `?force=`) |
| `GET /api/v1/volumes/{vol}/hosts[/{host}]` | `hosts` |
| `POST /api/v1/volumes/{vol}/rebalance` | `rebalance` (`dry_run`) → `202` |
| `POST /api/v1/volumes/{vol}/hosts/{host}/maintenance` | `maintenance start\|stop` → `202` |
| `GET /api/v1/volumes/{vol}/operations[/{id}]` | `status` |
| `GET /api/v1/volumes/{vol}/hosts/{host}/metrics` | `BLOCK_HOST_METRICS` (§4) |
| `GET /api/v1/version`, `/healthz` | версия apid, демона, capabilities |

- Асинхронные операции: `202 Accepted` + `Location: .../operations/{id}`; клиент
  опрашивает. Позже можно добавить `GET .../operations/{id}?wait=30`
  (long-poll).
- Запросы с подтверждением (`GB_CONFIRM_REQUIRED`): `409` с текстом демона и
  `confirm_token`; клиент повторяет запрос с `"confirm": true` (аналог `--yes`).
- Ошибки: `{"error": {"code", "message", "hosts": [...]}}`, HTTP-коды:
  `400/404/409/423` (том занят `meta.lock`) `/502` (хост не отвечает).
- **Идемпотентность для оператора/CSI**: create с тем же именем и
  параметрами → `200` с существующим блоком, с другими → `409`; delete
  отсутствующего → `204`. Необязательный заголовок `Idempotency-Key`.
- Схема ответа: свой стабильный JSON v1 (lowercase-ключи, числа, размеры в
  байтах). Его собирает apid из JSON демона. Так CLI-формат (`READ-MB`,
  строки) может меняться, а API нет (В10). Преобразование покрыто тестами на
  фикстурах JSON демона (§7.3).

### Для k8s

Этот API закрывает контроллерную часть CSI: CreateVolume / DeleteVolume /
ControllerExpandVolume / GetCapacity / ListVolumes. Оператор (или CSI
controller) получает список endpoint'ов apid (любой узел, координацию
делает демон) или VIP. Узловая часть (iSCSI login + multipath на k8s-нодах) —
забота CSI node plugin, не API. Сам оператор/CSI-драйвер — **не в этой
ветке**, отметить в BACKLOG.

### Технологии плагина (В1)

**React + PatternFly 5**, сборка esbuild по образцу
`cockpit-project/starter-kit` (так сделаны cockpit-podman,
cockpit-machines). Интерфейс на английском и русском (В7): gettext `po/`,
`cockpit.gettext`.

- Сборка JS только в CI / `make dist`; в RPM идёт готовый `dist/`, `rpmbuild`
  не требует node и сети (spec как у cockpit-podman: `Source0` = tarball с dist).
- Пакет `noarch`, файлы в `/usr/share/cockpit/gluster-block/`
  (`manifest.json`, `index.html`, `index.js`, `index.css`, `po.*.js`).

## 2. Размещение в репозитории

```
cockpit/
  PLAN.md                       этот файл (удалю/перенесу в BACKLOG по готовности)
  package.json, build.js        esbuild, eslint
  Makefile                      dist, rpm, devel-install (~/.local/share/cockpit)
  cockpit-gluster-block.spec.in
  src/
    manifest.json               меню "Storage" → "Gluster Block"
    index.html, app.jsx
    api.js                      клиент HTTP API v1 (cockpit.http, unix socket), ошибки, 202/409
    volumes.jsx, blocks.jsx, hosts.jsx, operations.jsx, metrics.jsx
    dialogs/ create, delete, resize, prio-path, maintenance, confirm
    prom.js                     парсер Prometheus text format (для метрик)
  test/                         unit-тесты api.js/prom.js на JSON-фикстурах
  test/mock-apid/               mock API v1 со сценариями для ручной проверки (§7.6)
  test/devel/                   dev-shim cockpit.js, Dockerfile стенда уровня 2
  po/                           переводы (en, ru)
```

- Версия пакета = версия gluster-block из `version-gen.sh` (тот же тег).
- `Requires: cockpit-bridge >= 264` (EL8.8+, EL9), `gluster-block-api >=
  <версия этой ветки>`.
- apid и `libgbclient` (§1a) лежат в дереве gluster-block (`api/`, `rpc/`),
  а не в `cockpit/`.
- `extras/make_rpms.sh` собирает и этот RPM; CI `rpm-build.yml` — новый шаг
  (node + `make dist` + rpmbuild).
- Только EL (В6): `gluster-block-api` и `cockpit-gluster-block` собираются как
  RPM. В Debian-пакете gluster-block apid пока не ставится, отметка в BACKLOG.

## 3. Функции UI

Главная страница: список block hosting volumes (имя, тип gluster-тома,
блоков, хостов, хосты в maintenance, проблемы, бейдж HEALTHY). Клик → страница
тома с вкладками.

### 3.1 Блоки: создать / удалить (п.1)

- Таблица: имя, размер, HA, хосты, prio-хост, состояние по хостам
  (`info --json`: CONFIGURED / *FAIL), GBID.
- **Создать**: диалог — имя, размер, хосты (мультивыбор из пиров gluster +
  хосты тома), HA (по числу хостов), `prealloc` (для dispersed принудительно
  `full`, как требует README), `ring-buffer`, `block-size`, `io-timeout`,
  auth (CHAP). Проверка имени/размера до отправки.
- `GB_CONFIRM_REQUIRED` (например thin на dispersed): показать текст демона и
  кнопку «Продолжить» → повтор с `--yes`.
- **Статус исполнения**: create асинхронный — ответ `IN-PROGRESS` + id
  операции; плагин опрашивает `status <vol> <id> --json` (1–2 с) и показывает
  фазу/шаг/прогресс в диалоге и в списке операций; диалог можно закрыть,
  операция видна во вкладке «Операции».
- **Удалить**: подтверждение вводом имени блока, опции `unlink-storage`,
  `force`. Delete становится асинхронным в демоне (В4, §6 п.3a): API отвечает
  `202` + операция, плагин показывает прогресс по хостам, как у create.

### 3.2 Изменить размер (п.2)

- Диалог: новый размер (единицы KiB…TiB), уменьшение только с `force` и явным
  предупреждением о потере данных (демон уже требует `force`).
- Асинхронно, прогресс через `status` (как create). После успеха — подсказка
  для инициатора (`iscsiadm -m node -R`, `multipathd resize map`).

### 3.3 Статус хостов тома (п.3)

- Вкладка «Хосты»: `hosts <vol> --json` — STATE (ONLINE / MAINTENANCE /
  ENTERING / LEAVING / DOWN / old-version), BLOCKS, LOADED, ENABLED, PRIO,
  версии демона, tcmu-runner, gluster-block-target, PROBLEMS.
- Раскрытие строки: `hosts <vol> <host> --json` — по каждому блоку бэкстор,
  TPG и enabled, ALUA, сессии инициаторов, SCSI-CMDS, READ/WRITE-MB.

### 3.4 Веса path'ов (п.4, п.6)

- Матрица «блок × хост» для тома: ячейка = AO (prio) / ANO / disabled /
  нет пути, с расхождением между метаданными (`PRIOPATH`) и configfs.
- Итог по хосту: число prio-путей (счётчики `prio.info`) и отклонение от
  равномерного.
- **Задать**: в ячейке или в диалоге блока — «сделать prio-путём»
  → `modify <vol>/<blk> prio-path <host> [force]` (с объяснением force при
  недоступных хостах).
- **Rebalance** тома: сначала `rebalance dry-run --json` — показать план
  перемещений, затем запуск с прогрессом через `status`.
- Числовых весов нет (В3, идея в BACKLOG).

### 3.5 Maintenance (п.5)

- В строке хоста: «Вывести в maintenance» / «Вернуть».
- Start: сначала без `force`; при отказе показать причины демона (последний
  рабочий путь, хост недоступен, занятые LUN/инициаторы по таймауту).
  `force` — отдельная кнопка с перечнем того, что пропускается
  (`GB_CONFIRM_REQUIRED` → `--yes`).
- Stop: опции `force`, `rebalance`; показать блоки с ошибками на хосте.
- Прогресс (ENTERING: перенос prio → ожидание простоя I/O → disable TPG) через
  `status`.

### 3.6 Операции

- Вкладка: `status <vol> --json` — текущая и последние операции
  (тип, начало, фаза, шаг, прогресс, результат, ошибка).

## 4. Метрики tcmu-runner по хостам (п.7)

Как сейчас в форке tcmu-runner (`feature/metrics`): поток-экспортёр пишет
`tcmu_runner.prom` (Prometheus text, атомарно через `.tmp` + rename) каждые
`metrics_interval` с; путь — `metrics_file` в `/etc/tcmu/tcmu.conf`; читает
node_exporter textfile collector. Плюс `lio-stats.sh` (timer) → `lio.prom`.
HTTP в tcmu-runner нет.

Файл лежит **на каждом хосте**, плагин работает на одном. Решение (В2):

**Через gluster-blockd.** Новая удалённая процедура
`BLOCK_HOST_METRICS` (новая, существующие структуры не трогаем) + capability
`host_metrics`. Хост читает свои `.prom` (пути: из `tcmu.conf`
`metrics_file`, `GB_METRICS_FILE`, опционально `lio.prom`), отдаёт текст,
отфильтрованный по блокам тома. CLI:
`gluster-block hosts <vol> [<host>] metrics [--json]`, API:
`GET .../hosts/{host}/metrics`. Работает через уже открытый порт 24010, без
node_exporter и ssh; хост без процедуры → `old-version`, без файла →
«метрики выключены».

Отклонено: читать node_exporter каждого хоста (`:9100`) — нужен node_exporter
везде и доступ к порту с узла с Cockpit.

UI (вкладка «Метрики», выбор хост / блок):
- по хосту и блоку: IOPS и MB/s read/write (дельта `tcmu_commands_total`,
  `tcmu_bytes_total` между опросами), латентность p50/p99 из дельты
  гистограммы `tcmu_command_duration_seconds`, inflight,
  ошибки по статусам (`hardware_error`, `read/write_error`, `timeout`),
  `tcmu_glfs_errors_total` по op/errno, `tcmu_in_recovery`, lock state,
  `tcmu_alua_access_state`, `tcmu_cmd_timed_out_total`, `tcmu_conn_lost_total`;
- свежесть: `tcmu_metrics_timestamp_seconds` (предупреждение, если старше
  3 интервалов), версия tcmu-runner;
- спарклайны за время открытой страницы (история только в браузере,
  долговременная — в Prometheus/Grafana, ссылку можно задать в настройках).

## 5. Собственные метрики gluster-blockd (п.8)

Та же схема, что в tcmu-runner: отдельный поток-экспортёр,
счётчики — атомарные инкременты, запись `<file>.tmp` + rename, удаление файла
при остановке, ошибки экспорта не фатальны. Настройки в
`/etc/sysconfig/gluster-blockd`:

```
#GB_METRICS_FILE="/var/lib/node_exporter/textfile_collector/gluster_block.prom"
#GB_METRICS_INTERVAL=15
```

Пусто — экспорт выключен. Метки: `volume`, `block` (+ `instance` от Prometheus).

### 5.1 Экспортёр и демон

| Метрика | Тип | Смысл |
|---|---|---|
| `gluster_block_info{version}` | gauge | 1, версия демона |
| `gluster_block_start_time_seconds` | gauge | время старта (рестарты) |
| `gluster_block_metrics_timestamp_seconds` | gauge | время записи файла |
| `gluster_block_metrics_write_errors_total` | counter | ошибки записи файла |
| `gluster_block_service_up{service}` | gauge | tcmu-runner, gluster-block-target, rpcbind, glusterd |

### 5.2 RPC и операции

| Метрика | Тип | Смысл |
|---|---|---|
| `gluster_block_requests_total{op,source,result}` | counter | запросы: `source`=cli/remote, `result`=ok/fail/confirm |
| `gluster_block_request_duration_seconds{op,source}` | histogram | длительность обработки |
| `gluster_block_remote_calls_total{peer,result}` | counter | вызовы других хостов: ok/fail/timeout/unreachable |
| `gluster_block_operations_running{op}` | gauge | идущие async-операции (create, modify_size, rebalance, maintenance) |
| `gluster_block_operations_total{op,result}` | counter | завершённые async-операции |
| `gluster_block_operation_duration_seconds{op}` | histogram | длительность async-операций |
| `gluster_block_targetcli_runs_total{result}` | counter | запуски targetcli |
| `gluster_block_targetcli_duration_seconds` | histogram | самое медленное место настройки LIO |

### 5.3 Метаданные и gluster

| Метрика | Тип | Смысл |
|---|---|---|
| `gluster_block_meta_lock_wait_seconds{volume}` | histogram | ожидание `meta.lock` (конкуренция узлов) |
| `gluster_block_meta_errors_total{volume,op}` | counter | ошибки gfapi по метаданным (read/write/lock/xattr) |
| `gluster_block_glfs_cache_total{result}` | counter | LRU кэш glfs: hit/miss/evict |
| `gluster_block_glfs_init_errors_total{volume}` | counter | ошибки подключения к тому |

### 5.4 Состояние тома и блоков на этом хосте

Читаются экспортёром из configfs и xattr (`hosts.info`, `prio.info`), не
через RPC; метаданные блоков — раз в `GB_METRICS_META_INTERVAL` (60 с), только
томов с блоками на хосте.

| Метрика | Тип | Смысл |
|---|---|---|
| `gluster_block_blocks{volume}` | gauge | блоков тома на хосте |
| `gluster_block_block_info{volume,block,gbid,ha}` | gauge | 1 на блок |
| `gluster_block_block_size_bytes{volume,block}` | gauge | размер |
| `gluster_block_block_prio{volume,block}` | gauge | 1 — prio-путь (AO) на этом хосте |
| `gluster_block_block_tpg_enabled{volume,block}` | gauge | TPG хоста включён |
| `gluster_block_block_loaded{volume,block}` | gauge | бэкстор загружен в tcmu-runner |
| `gluster_block_block_sessions{volume,block}` | gauge | залогиненные инициаторы |
| `gluster_block_block_meta_state{volume,block,state}` | gauge | состояние хоста в метаданных (CONFIGURED/*FAIL) |
| `gluster_block_block_hosts_failed{volume,block}` | gauge | хостов блока в *FAIL по метаданным |
| `gluster_block_host_maintenance{volume,state}` | gauge | NONE/ENTERING/MAINTENANCE/LEAVING этого хоста |
| `gluster_block_prio_paths{volume}` | gauge | prio-путей на хосте |
| `gluster_block_peer_up{volume,peer}` | gauge | пир тома отвечает на 24010 (проба 3 с) — основа для алертов и будущего автофейловера |

Дополнительно (как в tcmu-runner): `extras/monitoring/gluster-block.rules.yml`
(алерты: демон/tcmu-runner down, метрики устарели, блок без prio-пути,
пир недоступен, хост в maintenance дольше N часов, *FAIL в метаданных,
рост ошибок targetcli/meta), `extras/monitoring/prometheus.yml` (пример
scrape) и раздел в `docs/monitoring.md`.

Grafana сразу (В5): `extras/monitoring/gluster-block-dashboard.json`:
- обзор кластера: демоны и пиры up/down, хосты в maintenance, блоки с *FAIL,
  блоки без prio-пути, распределение prio-путей по хостам;
- операции: идущие и завершённые async-операции, длительность, ошибки;
  RPC и targetcli — частота, латентность, ошибки; ожидание `meta.lock`;
- по тому и блоку: состояние на хостах, сессии, prio; переменные
  `volume`, `block`, `instance`, со ссылкой на дашборд tcmu-runner с теми же
  фильтрами (метка `device` = блок).

Файлы мониторинга ставятся в `/usr/share/gluster-block/monitoring/`.

## 6. Изменения в gluster-block (демон/CLI)

Нужны для API, п.7, п.8 и В4:
1. `libgbclient`: вынести из `cli/gluster-block.c` подключение к сокету и
   вызовы `GLUSTER_BLOCK_CLI`; CLI и apid используют её.
2. `gluster-block-apid` (§1a): `api/`, systemd unit, sysconfig, подпакет
   `gluster-block-api` в spec, OpenAPI, README.
3. Новая CLI-процедура `volumes` (список gluster-томов с признаком block
   hosting: демон вызывает `gluster volume list --xml` и проверяет
   `/block-meta`). Она нужна API, а CLI получит `gluster-block volumes`.
3a. Асинхронный delete (В4): координатор проверяет синхронно, потом
   `gbAsyncRun()`, фазы и шаги по хостам в `status`. Межузловой
   `BLOCK_DELETE` не меняется, новая capability не нужна. Чтобы не сломать
   скрипты, CLI `delete` по умолчанию ждёт конец операции, как сейчас; новый
   флаг `--no-wait` возвращает `IN-PROGRESS` + id. API всегда отвечает `202`.
   Демон 0.5.33 на координаторе в окне не участвует: apid всегда говорит с
   демоном той же версии (§7.5).
4. `BLOCK_VERSION_CLI` (§7.2): версия, `cli_rpc_rev`, capabilities по хостам.
4a. Проверки совместимости (§7.3): эталоны RPC, JSON-схемы, oasdiff,
   смешанный кластер в CI.
4b. Экспортёр метрик (§5): `daemon/` + `utils/metrics.{c,h}`, sysconfig, README.
5. `BLOCK_HOST_METRICS` + capability `host_metrics` + `hosts ... metrics` (§4 А).
6. Проверить, что все нужные команды дают полный `--json`
   (`list`, `info`, `status`, `hosts`, `rebalance dry-run`, ошибки и
   CONFIRM) — дополнить, где не хватает полей для API.
7. README, help CLI, NEWS, `%changelog`, `BASE_VERSION`.

## 7. Тесты и совместимость

### 7.1 Где может сломаться

```
плагин ──(1) HTTP API v1──> apid ──(2) RPC GLUSTER_BLOCK_CLI + JSON демона──> gluster-blockd ──(3) RPC GLUSTER_BLOCK──> другие хосты
```

| Стык | Что обновляется отдельно | Чем защищаемся |
|---|---|---|
| (1) плагин ↔ apid | `cockpit-gluster-block` и `gluster-block-api` — разные RPM | контракт OpenAPI v1 + диапазон версий в RPM + проверка версии в рантайме |
| (2) apid ↔ локальный демон | один spec, но работающий демон может быть старым, пока его не перезапустили | `Requires: gluster-block = %{version}-%{release}` + handshake при старте apid |
| (3) демон ↔ демоны других хостов | хосты кластера обновляются по одному | правило «не менять существующие структуры» + capabilities, теперь с проверкой в CI |

### 7.2 Рантайм: версия вместо падения

- Новая CLI-процедура `BLOCK_VERSION_CLI` (`= 15`). Она возвращает версию
  демона, `cli_rpc_rev` (ревизию схемы CLI-RPC), capabilities локального
  демона и **минимальные capabilities по хостам каждого тома** (из того, что
  уже собирает `hosts`).
- apid вызывает её при старте и после каждой ошибки соединения (демон
  перезапустился). Если ревизия неизвестна, `GET /healthz` отдаёт
  `503 daemon-incompatible`, мутирующие запросы — `503` с понятным текстом.
- `GET /api/v1/version` возвращает `api_version` (`1.<minor>`), версии apid и
  демона, список `features` (например `prio_path`, `host_maintenance`,
  `host_metrics`, `volumes`) с учётом всех хостов тома.
- Плагин при загрузке читает `/version`:
  - другой major API → баннер «несовместимая версия gluster-block-api»,
    только чтение;
  - нет нужного `feature` (старый хост в томе) → кнопка неактивна с
    подсказкой «хост X: gluster-block 0.5.27, нужно ≥ 0.5.33». Без
    исключений в JS.
- Неизвестные поля в ответах плагин игнорирует, а отсутствующие
  необязательные поля переживает. Это проверяется тестами 7.4.

### 7.3 CI gluster-block: RPC и JSON не ломаются молча

1. **Снимок RPC-схемы** (`tests/compat/rpc/`): скрипт
   `tests/compat/rpc-check.sh` разбирает `rpc/rpcl/block.x` и сравнивает с
   эталонами, которые лежат по одному на каждый релиз:
   - существующие процедуры (номер, аргумент, результат) и **все поля их
     структур** обеих программ не меняются. Можно добавлять новые процедуры
     и структуры;
   - если меняется CLI-структура, нужен явный новый
     `GLUSTER_BLOCK_CLI_VERS` или новая процедура, и тогда тест требует
     обновить эталон и `cli_rpc_rev`;
   - плюс XDR-проверка: бинарь, собранный из эталонного `block.x`,
     кодирует запрос, текущий декодирует (и наоборот). Так ловятся
     изменения типов, которые парсер мог пропустить.
2. **JSON-схемы ответов демона** (`tests/compat/json/*.schema.json`) для
   `list`, `info`, `status`, `hosts`, `rebalance dry-run`, `volumes`,
   `metrics`, ошибок и CONFIRM. Удалять или переименовывать поле и менять
   тип нельзя, добавлять можно.
3. **Контракт API**: `docs/api/openapi.yaml` + `oasdiff breaking` против
   OpenAPI последнего релиза. Ломающее изменение v1 = красный CI.
4. **Функциональный тест в контейнере** (Rocky, fake configfs, netns-хосты,
   как в CLAUDE.md): поднимаются демон и apid, прогоняются create /
   resize / delete / prio-path / rebalance / maintenance через HTTP. Каждый
   ответ проверяется по OpenAPI, сырой JSON демона — по схемам из п.2.
   Заодно ответы пишутся в фикстуры для плагина (7.4).
5. **Смешанный кластер**: в тех же netns один «хост» запускается с
   gluster-blockd **минимальной поддерживаемой версии 0.5.33** (В13) и один
   с **0.5.27** (ниже окна). Оба собираются в job из тегов `v0.5.33` /
   `v0.5.27` (В14), сборки кэшируются в CI по тегу. Проверяется, что вся
   функциональность плагина работает с хостом 0.5.33, хост 0.5.27 виден как
   old-version, `/version` убирает недоступные `features`, а новые операции
   отказывают до изменений (capabilities), а не падают посередине. Когда
   появятся новые релизы, в матрицу добавляется ещё последний.

### 7.4 CI плагина: каждая сборка против нескольких версий API

- **Матрица версий gluster-block**: `min` (первый релиз с apid, указанный в
  `Requires` плагина), `latest` (последний тег) и `master`. Каждая версия
  собирается в job из своего тега (В14, с кэшем по тегу). Из неё берутся
  `openapi.yaml`, а фикстуры снимаются функциональным тестом (7.3 п.4) этой
  же версии. Пока релиз с apid один, `min` = `latest`.
- **Unit-тесты** `api.js`/`prom.js` (node, без браузера) гоняются на
  фикстурах каждой версии матрицы; ответы проверяются по OpenAPI этой версии.
- **Mock-сервер из OpenAPI** (prism) для каждой версии матрицы; UI-смоук в
  Playwright против cockpit-ws в контейнере. Каждая страница открывается,
  а действия, которых нет у старой версии, неактивны и не дают ошибок в
  консоли.
- **E2E**: тот же контейнер, что в 7.3 п.4, с реальными демоном и apid для
  `latest` и `master`. Playwright проходит create → resize (с прогрессом) →
  prio-path → maintenance start/stop → delete.
- Если `master` gluster-block ломает плагин, CI плагина краснеет, и
  ломается это до релиза gluster-block. Поэтому job плагина также
  запускается в CI gluster-block на каждом PR (плагин в том же репозитории).

### 7.5 Упаковка

- `gluster-block-api`: `Requires: gluster-block = %{version}-%{release}` (apid и
  демон из одного дерева, рассинхрона на одном хосте нет). При upgrade
  `%systemd_postun_with_restart` перезапускает оба сервиса.
- `cockpit-gluster-block`: `Requires: gluster-block-api >= <min>` и
  `Conflicts: gluster-block-api >= <следующий major API>`. Диапазон тот же,
  что `min`/`master` в матрице 7.4.
- Окно совместимости (В13): плагин работает с API текущего major, а на
  других хостах тома — с gluster-blockd **>= 0.5.33**. Более старые хосты
  видны как old-version, управление ими заблокировано.

### 7.6 Ручная проверка UI/UX

Три уровня стенда, от быстрого к реальному. Все запускаются из `cockpit/Makefile`.

**1. `make dev` — браузер на Mac, без Cockpit и кластера (секунды).**
- esbuild в режиме watch + `test/mock-apid/` — небольшой node-сервер,
  который реализует API v1 по OpenAPI на данных из фикстур (§7.3 п.4).
- Shim `cockpit.js` для dev: `cockpit.http` → `fetch` к mock-серверу,
  `cockpit.gettext` и переключение языка en/ru, светлая/тёмная тема
  PatternFly. В RPM shim не попадает.
- **Сценарии** выбираются в выпадающем dev-меню или через `?scenario=`:
  - `healthy`: 2 тома, 3 хоста, 20 блоков;
  - `big`: 500 блоков, 8 хостов, для таблиц, матрицы путей и производительности;
  - `host-down`: хост не отвечает, блоки без prio-пути;
  - `maintenance`: хост в ENTERING с прогрессом, отказ по busy LUN, CONFIRM
    при force;
  - `mixed-versions`: хост 0.5.27 old-version, неактивные кнопки;
  - `errors`: 409/423/502/503, apid недоступен, демон несовместим;
  - `slow`: задержки 2–5 с, проверка спиннеров и таймаутов.
- В mock **живые асинхронные операции**: create, resize, delete, rebalance и
  maintenance идут по фазам во времени, можно отменить сценарий посередине
  или «уронить» хост кнопкой. Метрики — синтетические `.prom`, счётчики растут.
- Изменения в коде видны сразу (live reload).

**2. `make devel-container` — настоящий Cockpit в docker (минуты).**
- Rocky 9 (arm64 и x86_64) с `cockpit-ws`, `cockpit-bridge`, плагином из
  `dist/` (bind mount, пересборка без перезапуска) и mock-apid на
  unix-сокете `/run/gluster-block/api.sock`. Открывается
  `https://localhost:9090`, пользователь `admin`.
- Проверяется то, чего нет в уровне 1: меню и оболочка Cockpit, superuser,
  переходы, реальный `cockpit.http` через unix-сокет, sandbox и CSP Cockpit.
- Флаг `BACKEND=fake-cluster` вместо mock поднимает настоящие gluster-blockd +
  apid на fake configfs с netns-хостами (стенд функционального теста
  §7.3 п.4). Тогда операции выполняет настоящий демон.

**3. `make devel-install` / `RSYNC=<node> make watch` — ваш кластер.**
- `devel-install` ставит ссылку на `dist/` в `~/.local/share/cockpit/gluster-block`
  на узле. Cockpit берёт плагин из домашнего каталога раньше системного,
  RPM не нужен.
- `RSYNC=<node> make watch` (как в starter-kit) после каждой сборки
  копирует `dist/` на узел. Обновляете страницу и видите реальные данные.
- `make devel-uninstall` убирает ссылку и возвращает версию из RPM.

**Скриншоты на каждый PR.** Playwright-смоук (§7.4) сохраняет скриншоты
каждого экрана и диалога во всех сценариях уровня 1, в en/ru и светлой/тёмной
теме. Их можно посмотреть в артефакте CI, не запуская стенд. Сравнение с
прошлым PR показывает, что изменилось визуально (без падения CI, только для
ревью).

**Точки ревью UI** (стоп-кадры в этапах §8): после этапа 3 (каркас и экраны
чтения на mock) и после этапов 4 и 5 (диалоги). На каждой точке вы смотрите
стенд уровней 1–2, даёте замечания, я правлю в том же коммите этапа до перехода
к следующему.

### 7.7 Остальное

- `-Werror` сборка, eslint, unit-тесты `prom.js` на `.prom` фикстурах с
  реального кластера (tcmu-runner `feature/metrics` и gluster-blockd).
- Финал — 3-узловой кластер пользователя: create/resize с прогрессом,
  prio-path, rebalance, maintenance start/stop под нагрузкой, метрики,
  обновление одного хоста на новую версию при работающем плагине.
- CI: сборка RPM `gluster-block-api` и `cockpit-gluster-block` на
  Rocky/Alma 8/9, CS9.

## 8. Этапы и коммиты

Для этой ветки — **один коммит на этап** (В12, исключение из правила «одна
ветка = один коммит»). Каждый коммит собирается с `-Werror`, проходит CI и
обновляет README/help своих команд. Версия (`BASE_VERSION`, NEWS,
`%changelog`) поднимается в последнем коммите, по числу решённых проблем.

0. `tests: RPC compatibility check` — эталон `block.x` текущего релиза 0.5.33,
   `rpc-check.sh`, XDR-проверка, job в CI (§7.3 п.1). До любых изменений RPC.
   **Готово:** эталоны v0.5.27 и v0.5.33 (из тегов), XDR сравнивается по
   функциям, которые генерирует rpcgen, `--self-test` на 13 случаях, `make check` во всех
   CI-сборках.
1. `api: gluster-block-apid with read-only API` — `libgbclient`, `volumes`,
   `BLOCK_VERSION_CLI`, apid на unix-сокете, эндпоинты чтения, `/version`,
   v1 JSON, OpenAPI, JSON-схемы, подпакет `gluster-block-api`, функциональный
   тест в контейнере, oasdiff.
   **Готово.** Уточнения по ходу работы:
   - `gluster-block version` без аргументов печатает версию CLI, как раньше,
     а `version --json` и `version <vol>` опрашивают демон (`CLI-RPC-REV` = 16).
   - В JSON `info` добавлены `SIZE-BYTES`, `PRIO-PATH`, `RING-BUFFER-MB`, `BLOCK-SIZE`,
     `IO-TIMEOUT-SEC`, `PREALLOC`, `ENTRY`, `HOST-STATES`: без них v1 не построить.
   - Все команды, кроме `create`, создавали `/block-meta` и `meta.lock` на
     томе, о котором их спросили (`list`, `info`, `hosts`, `delete`...). Исправлено:
     метаданные создаёт только `create` (`glusterBlockOpenMetaLockFile`), `list` и
     `hosts` на таком томе пусты, остальные отвечают ENOENT. apid ещё отвечает
     404 `not_block_hosting`, чтобы клиент отличал такой том от пустого.
   - На томе одна долгая операция за раз, остальные получают EBUSY → 423
     `volume_busy`.
   - CLI-сервер демона однопоточный, параллельные запросы apid ждут друг
     друга (как команды CLI). `volumes` кэшируется на 15 с.
   - Стенд `tests/functional/` (`run.sh`): glusterd, 3 хоста в netns, у каждого
     свой configfs, который строит заглушка targetcli. В CI это отдельный workflow
     `functional.yml` (+ job `API contract`). GitLab без privileged — там его нет.
2. `api: write operations, async delete, TLS` — мутирующие эндпоинты,
   202/операции, 409/confirm, идемпотентность, асинхронный delete в демоне
   (+ `--no-wait`), TCP + TLS + mTLS/токены, аудит.
3. `cockpit: plugin skeleton and read-only views` — каркас React/PF5, RPM
   `cockpit-gluster-block`, CI, матрица версий (§7.4), тома и блоки,
   переводы en/ru, dev-стенды уровней 1–3 с mock-apid и сценариями (§7.6),
   скриншоты в CI. **Точка ревью UI.**
4. `cockpit: create, delete and resize with progress` — диалоги, confirm,
   вкладка операций, Playwright-сценарии. **Точка ревью UI.**
5. `cockpit: hosts, prio paths and maintenance` — хосты, матрица путей,
   prio-path, rebalance, maintenance start/stop. **Точка ревью UI.**
6. `daemon: Prometheus metrics of gluster-blockd` — экспортёр (§5), правила
   алертов, Grafana-дашборд, `docs/monitoring.md`.
7. `metrics: per host tcmu-runner and gluster-blockd metrics` —
   `BLOCK_HOST_METRICS` + capability, `hosts ... metrics`, эндпоинт API,
   вкладка метрик в плагине, тест смешанного кластера (§7.3 п.5).
8. `release: 0.5.N` — README/docs итог, BACKLOG, удаление этого PLAN.md,
   версия. Перед ним — проверка на 3-узловом кластере.

## 9. Решения ревью (2026-11-26)

| | Вопрос | Решение |
|---|---|---|
| В1 | Фреймворк плагина | React + PatternFly 5 + esbuild, node только для dist |
| В2 | Метрики с других хостов | через gluster-blockd: `BLOCK_HOST_METRICS` |
| В3 | Веса путей | бинарная модель AO/ANO; числовой вес хоста — в BACKLOG |
| В4 | Delete | асинхронный, с прогрессом |
| В5 | Grafana-дашборд gluster-blockd | в этой ветке |
| В6 | Debian | пока только EL |
| В7 | Переводы | en + ru |
| В8 | Демон в той же ветке | снят, см. В12 |
| В9 | Доступ к unix-сокету apid | только root (Cockpit с superuser) |
| В10 | Схема API | свой стабильный JSON v1 из JSON демона |
| В11 | Аутентификация TCP | mTLS (оператор) + токены (скрипты) |
| В12 | Объём ветки | всё в `cockpit-plugin`, коммит на этап (§8) |
| В13 | Окно совместимости | хосты gluster-blockd >= 0.5.33 |
| В14 | Старые версии для CI | собирать из тега в job |
