# HA 슬레이브 만들기 POC — CMS task 규격 (2안)

POC 전용. 슬레이브가 될 서버의 CMS에 task 5개를 새로 둠. 스크립트(ha_make_slavedb.sh)는 쓰지 않음.
마스터 쪽 작업은 CMS에 링크한 ssh 라이브러리(libssh)로 하고, 시스템 ssh·scp 명령은 부르지 않음.
마스터 CMS와 엔진은 바꾸지 않음.

## 공통

- 모두 admin 전용(AU_ADMIN). 요청·응답은 CMS JSON 그대로(`task`, `token`, 응답 `status`·`note`).
- 비밀번호 키 이름은 `os_password`(NCA 로그 가림 규칙: 키에 `pass` 포함).
- 비밀번호는 CMS 메모리에만 두고 작업이 끝나면 지움. 명령줄 인자·로그·상태 파일·비동기 요청 사본(gettaskstatus 쪽)에 남기지 않음.
  이 task들은 CMS의 기존 async 틀(`"async":"yes"`, gettaskstatus)을 쓰지 않고 자체 작업 목록을 씀.
- 호스트 키 지문 형식은 OpenSSH와 같음: `SHA256:` + base64(패딩 없음).
- 리눅스 빌드만. 윈도우 빌드에는 이 task들이 없음.

## 사전 조건(슬레이브)

- `start_ha=yes`이면 슬레이브에 cub_master가 HA가 아닌 상태로 떠 있으면 안 됨. cub_master는 시작할 때만 ha_mode를 읽어서, 그 상태로는 `cubrid heartbeat start`가 실패함. CMS가 service에 묶여 있어 task가 service를 다시 띄울 수는 없으므로, 미리 `cubrid service stop` 후 `cubrid manager start`로 CMS만 띄워 두어야 함. 이 조건은 precheck가 확인함(`precheck_failed`).
- 슬레이브의 그 DB 서버는 꺼져 있어야 함(`slave_db_running`).

## 1. `ha_check_ssh` — ssh 연결 확인 (동기, 최대 약 20초)

요청

| 키 | 필수 | 뜻 |
|---|---|---|
| `host` | 예 | 마스터 호스트 이름 또는 IP |
| `ssh_port` | 아니오 | 기본 22 |
| `os_user` | 예 | 마스터 OS 계정(CUBRID 운영 계정) |
| `os_password` | 지문 확인 뒤 | 마스터 OS 계정 비밀번호 |
| `hostkey_sha256` | 아니오 | 사용자가 확인한 지문. 없거나 실제 지문과 다르면 비밀번호를 보내지 않고 멈춤 |

응답 (`status`는 확인을 끝까지 돌렸으면 `success`, 입력 오류·내부 오류면 `failure`)

| 키 | 뜻 |
|---|---|
| `result` | `ok` / `unreachable`(이름 풀이·접속·포트) / `hostkey_unknown`(지문 미확인, 지문을 보여 주고 다시 부르면 됨) / `hostkey_mismatch`(넘긴 지문과 다름) / `auth_failed` / `cubrid_not_found` / `version_mismatch` / `error` |
| `message` | 사람이 읽을 실패 설명 |
| `hostkey_type`, `hostkey_sha256` | 마스터 호스트 키 종류·지문(접속만 되면 항상) |
| `remote_hostname` | 마스터 `uname -n` |
| `remote_cubrid_rel` | 마스터 `cubrid_rel` 첫 줄 |
| `remote_version`, `local_version` | 예: `11.4.5.1908`. 다르면 `version_mismatch` |
| `remote_cubrid`, `remote_cubrid_databases` | 마스터 `$CUBRID`, `$CUBRID_DATABASES`(로그인 셸 기준) |

흐름: 1회차는 지문 없이 불러 `hostkey_unknown`+지문을 받고, 사용자가 지문을 확인하면 지문과 비밀번호를 넣어 2회차를 부름.

## 2. `ha_make_slave` — 슬레이브 만들기 시작 (즉시 응답, 뒤에서 실행)

요청

| 키 | 필수 | 뜻 |
|---|---|---|
| `db_name` | 예 | HA DB 이름(마스터에서 active여야 함) |
| `master_host` | 예 | 마스터 노드 이름. cubrid_ha.conf `ha_node_list`에 쓰는 이름과 같아야 하고 슬레이브에서 이름 풀이가 되어야 함 |
| `ssh_port` | 아니오 | 기본 22. 접속 주소는 `master_host` |
| `os_user`, `os_password`, `hostkey_sha256` | 예 | `ha_check_ssh`와 같음 |
| `slave_host` | 아니오 | 이 서버의 노드 이름. 기본 `uname -n` |
| `repl_log_home` | 아니오 | 슬레이브 복제 로그 상위 폴더. 기본 `$CUBRID_DATABASES` |
| `master_backup_path` | 아니오 | 마스터에서 백업을 둘 폴더. 기본 마스터 `$CUBRID_DATABASES/ha_make_slave_backup`. 이 아래 작업 uuid 폴더를 만들고 끝나면 지움 |
| `slave_backup_path` | 아니오 | 슬레이브에서 받은 백업을 둘 폴더. 기본 `$CUBRID_DATABASES/ha_make_slave_backup`. 위와 같음 |
| `start_ha` | 아니오 | `yes`(기본)면 마지막에 슬레이브 HA 시작과 확인까지 함, `no`면 절차 9·10은 사용자가 함 |
| `replicas` | 아니오 | 레플리카 목록 자리(원소: `host`, `ssh_port`, `os_user`, `os_password`, `hostkey_sha256`). POC는 비어 있어야 함. 값이 있으면 `failure`("replicas are not supported in this POC") |

응답: `uuid`(작업 번호, 문자열), `job_status`=`running`. 다른 작업이 진행 중이면 `failure`, `note`에 그 uuid.

입력 값 검사: 호스트·DB 이름은 `[A-Za-z0-9._-]`만, 경로는 절대 경로이고 `[A-Za-z0-9._/-]`만, 포트는 1~65535.

## 3. `ha_make_slave_status` — 진행 조회

요청: `uuid`, `offset`(이미 받은 진행 기록의 바이트 위치, 기본 0)

응답

| 키 | 뜻 |
|---|---|
| `job_status` | `running` / `success` / `fail` / `cancelled` / `interrupted`(CMS가 재시작돼 작업이 끊김. 상태 파일에서 읽은 결과) |
| `phase` | `run` / `rollback`(실패·취소 뒤 되돌리는 중) / `done` |
| `step`, `step_no`, `step_count` | 지금 단계 id와 번호 |
| `steps` | 단계 배열. 원소: `id`, `no`, `title`, `state`(`pending`/`running`/`done`/`failed`/`skipped`/`rolled_back`/`rollback_failed`), `detail` |
| `output`, `next_offset` | `offset` 뒤에 새로 쌓인 진행 기록(사람이 읽는 줄 단위 텍스트)과 다음 위치 |
| `error` | 실패 이유 코드(아래) |
| `message` | 실패 설명 |
| `exit_code` | 실패한 명령의 종료 코드(명령 실패일 때) |
| `kept_db_paths` | 옮겨 보관한 기존 슬레이브 DB 폴더(사용자가 확인 뒤 지움) |
| `started_at`, `ended_at` | epoch 초 |

`error` 코드: `precheck_failed`, `unreachable`, `hostkey_mismatch`, `auth_failed`, `version_mismatch`, `master_not_active`, `slave_db_running`, `command_failed`, `transfer_failed`(크기·체크섬 불일치 포함), `config_edit_failed`, `verify_failed`, `cancelled`, `internal`.

## 4. `ha_make_slave_cancel` — 취소

요청: `uuid`. 응답: `job_status`(보통 `running`, `phase`=`rollback`으로 바뀜).
지금 돌고 있는 명령(백업·복원·복사)을 멈추고 끝낸 단계를 거꾸로 되돌림. 마스터 백업은 마스터 DB에 영향이 없으므로 멈추기만 함.

## 5. `ha_make_slave_cleanup` — 옮겨 보관한 옛 DB 지우기

요청: `uuid`(끝난 작업). 응답: 지운 경로. 성공한 작업의 `kept_db_paths`만 지움.

## 단계 (`steps` 순서)

| no | id | 어디서 | 하는 일 | 확인 | 되돌리기 |
|---|---|---|---|---|---|
| 1 | `precheck` | 슬레이브 | 입력 검사, 로컬 버전·환경, 슬레이브 DB가 꺼져 있는지 | 실패하면 종료(바뀐 것 없음) | 없음 |
| 2 | `connect` | 마스터 | ssh 접속, 지문 비교, 비밀번호 로그인 | 지문·인증 | 없음 |
| 3 | `master_check` | 마스터 | 마스터 `$CUBRID`·버전 비교, `cubrid changemode <db>@localhost`가 active, cubrid_ha.conf·databases.txt에 DB 있음, 마스터 노드 목록 읽기 | 위 조건 | 없음 |
| 4 | `slave_config` | 슬레이브 | 절차 1: cubrid.conf `ha_mode=on`, cubrid_ha.conf(`ha_port_id`, `ha_node_list`=마스터 목록+슬레이브, `ha_db_list`에 DB, `ha_copy_sync_mode`는 마스터 값을 노드 수만큼 늘림), databases.txt에 그 DB 줄(볼륨·로그 경로는 마스터 값, 호스트 `마스터:슬레이브`). 항목 단위로 고치고 `.bak.<uuid>` 남김 | 다시 읽어 값 확인 | `.bak`로 복원 |
| 5 | `master_backup` | 마스터 | `cubrid backupdb -C -D <폴더> --no-check <db>@localhost` | 종료 코드 | 백업 폴더 지움 |
| 6 | `fetch_backup` | 마스터→슬레이브 | SFTP로 백업 폴더 파일 전부와 `<db>_bkvinf` 받기. 끊기면 받은 자리부터 이어 받음 | 파일마다 크기와 sha256(마스터에서 계산한 값과 비교) | 받은 파일 지움 |
| 7 | `keep_old_db` | 슬레이브 | 기존 슬레이브 DB 볼륨·로그 폴더와 복제 로그 폴더가 있으면 `<경로>.ha_make_slave_<uuid>`로 옮겨 보관 | 옮겨졌는지 | 제자리로 옮김 |
| 8 | `restore` | 슬레이브 | `cubrid restoreslave -s master -m <마스터> -B <받은 폴더> <db>` | 종료 코드 | 새로 만든 볼륨·로그 지움 |
| 9 | `copylog` | 슬레이브 | `cub_admin copylogdb -L <repl_log_home>/<db>_<마스터> -m async --start-page-id=-1 <db>@<마스터>` | 종료 코드 | 복제 로그 폴더 지움 |
| 10 | `master_config` | 마스터 | 절차 6·8: 마스터 cubrid_ha.conf `ha_node_list`에 슬레이브 추가(`ha_copy_sync_mode`도 노드 수만큼 늘림. 모자라면 reload가 "Invalid Parameter. ha_copy_sync_mode"로 실패함), databases.txt 그 DB 줄 호스트에 슬레이브 추가. 항목 단위로 고치고 `.bak.<uuid>` 남김. 이어서 `cubrid heartbeat reload` | 다시 읽어 값 확인, 명령 출력 | `.bak`로 복원 후 reload |
| 11 | `replication_start` | 마스터 | 절차 7: `cubrid heartbeat replication start <슬레이브>`(마스터에 슬레이브용 copylogdb와 applylogdb를 띄움) | 명령 출력 | `cubrid heartbeat replication stop <슬레이브>`(10단계 되돌리기보다 먼저. 노드 목록에서 빠진 뒤에는 stop이 실패하면서도 마지막 줄은 success로 끝나므로 출력에서 `: fail`을 봄) |
| 12 | `slave_ha_start` | 슬레이브 | 절차 9: `cubrid heartbeat start <db>`(`start_ha=no`면 건너뜀) | 명령 출력 | `cubrid heartbeat stop <db>` |
| 13 | `verify` | 슬레이브 | 절차 10: `cubrid changemode <db>@localhost`가 standby가 될 때까지 최대 120초(`start_ha=no`면 건너뜀) | standby | 없음 |
| 14 | `cleanup` | 양쪽 | 마스터·슬레이브의 이번 작업 백업 폴더 지움. 옮겨 보관한 옛 DB는 남김 | 지워졌는지 | 없음 |

- 11단계가 필요한 이유(실측, 11.4.5.1908): `heartbeat reload`는 노드 목록만 다시 읽고 프로세스를 띄우지 않음. 그래서 reload만 하면 정방향 복제는 되지만, failover 뒤 옛 마스터가 다시 붙을 때 그 직후에 새 active에 쓴 데이터가 빠짐. 마스터에서 `copylogdb start`만 한 경우도 마찬가지로 빠짐. applylogdb는 처음 뜰 때 복사된 로그의 현재 끝부터 적용하기 때문임(log_applier.c 2045~2101). replication start로 applylogdb까지 미리 띄워야 빠지지 않음.
- 되돌리기: 실패하거나 취소되면 바꾼 것이 있는 단계(4~12)만 거꾸로 되돌림. 1~3·13·14처럼 되돌릴 것이 없는 단계는 상태(`done`/`failed`)를 그대로 둠. 취소 뒤에도 되돌리기 명령은 취소되지 않음.
- 마스터 명령은 ssh 라이브러리의 명령 실행으로 `bash -lc '...'`(로그인 셸, CUBRID 환경 변수 읽기)로 돌림. 비밀번호는 명령에 들어가지 않음.
- 설정 파일은 SFTP로 읽어 CMS 안에서 그 항목만 고친 뒤 임시 파일로 쓰고 이름을 바꿈.
- 상태 파일: `$CUBRID/tmp/ha_make_slave/<uuid>/state.json`(단계별 상태, 옮긴 경로, `.bak` 이름. 비밀번호 없음). 진행 기록은 메모리에 최대 1 MB, 넘치면 앞부분을 버리고 `next_offset`은 계속 늘어남.
- 한 번에 작업 하나. 끝난 작업은 다음 작업을 시작하기 전까지 조회할 수 있음.

## POC에서 하지 않는 것

- 레플리카(입력 자리만 있음).
- 두 서버의 변경을 한 번에 성공시키거나 한 번에 취소하는 것. 되돌리기는 단계별로 최선을 다할 뿐임.
- 마스터 CMS로 같은 설정 파일을 동시에 저장하는 경우의 충돌 방지.
- 윈도우.
