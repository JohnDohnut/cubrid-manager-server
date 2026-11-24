# HA 슬레이브 추가 POC — 마스터 CMS 실행형 규격 (2안, v2)

마스터(M)의 CMS에서 실행해 새 서버(NS)를 슬레이브로 만드는 POC. 동작은 둘로 나뉨.

- (1) HA 마스터 전환(`ha_convert_master`): 싱글 DB인 M을 노드 1개짜리 HA 마스터로 바꿈. M DB가 재시작됨.
- (2) 슬레이브 추가(`ha_make_slave`): M이 HA 마스터라는 전제에서 NS를 슬레이브로 만듦. M의 CMS task가 NS와 레플리카를 원격으로 다룸.

NCA 위자드는 M이 싱글이면 (1)을 부르고 끝나면 (2)를 이어서 부름. (1)만 끝나고 멈춰도 M은 정상 HA 마스터로 남음.

- M 쪽 작업은 로컬에서 함. NS와 레플리카 쪽 작업은 M의 CMS에 정적으로 링크한 ssh 라이브러리(libssh2)로 함. 시스템 ssh·scp 명령은 쓰지 않음.
- NS에는 CMS가 없어도 됨. NS와 레플리카의 CMS, 엔진은 바꾸지 않음.
- 스크립트(ha_make_slavedb.sh)는 쓰지 않음.
- 이전 판(새 슬레이브의 CMS가 실행하는 방식)은 같은 브랜치의 a641627에 있음.

POC 조건: Rocky 8, CUBRID 11.4.5.1908. 시험 구성은 (1) HA 마스터 M + 레플리카 R에 NS 추가, (2) 싱글 DB M을 HA로 바꾸고 NS 추가. 각 서버의 OS 계정 아이디·비밀번호만 주어짐.

## 공통

- 모두 admin 전용(AU_ADMIN). 요청·응답은 CMS JSON 그대로(`task`, `token`, 응답 `status`·`note`).
- 비밀번호 키 이름은 `os_password`(NCA 로그 가림 규칙: 키에 `pass` 포함). 레플리카 원소 안에서도 같은 이름.
- 비밀번호는 M의 CMS 메모리에만 두고 작업이 끝나면 지움. 명령줄 인자·로그·상태 파일·비동기 요청 사본에 남기지 않음. 이 task들은 CMS의 기존 async 틀을 쓰지 않음.
- 호스트 키 지문 형식은 OpenSSH와 같음: `SHA256:` + base64(패딩 없음). libssh2가 고르는 키는 보통 ecdsa임.
- 숫자 값(`ssh_port`, `offset`)은 문자열과 숫자 둘 다 받음.
- 빌드: libssh2 1.11.1과 OpenSSL 3.5.7을 ExternalProject로 소스에서 정적으로 빌드함(sha256 고정, 이전과 같음). 리눅스 빌드만 해당함.

## 사전 조건

M(이 CMS가 도는 서버)
- 슬레이브 추가: 그 DB가 HA 마스터로 떠 있어야 함(`cubrid changemode <db>@localhost`가 active, cubrid_ha.conf의 `ha_db_list`에 그 DB가 있음). 싱글이면 `master_not_ha`로 거절하고, 먼저 HA 마스터 전환을 하라고 알림.
- HA 마스터 전환: 그 DB가 싱글로 떠 있어야 하고, M에 다른 DB 서버가 떠 있으면 안 됨(아래 0절).
- `master_host`(M의 노드 이름)는 M의 `uname -n`과 같아야 하고, NS와 레플리카에서 그 이름이 풀려야 함.
- 마스터의 `ha_replica_list`에 있는 레플리카는 모두 `replicas`로 넘겨야 함.
- 마스터 보관 로그 보존(`log_max_archives` 등)은 사용자가 미리 맞춰 둠.

NS(새 슬레이브)
- sshd가 떠 있고, 입력한 OS 계정이 CUBRID 운영 계정이어야 함. 로그인 셸에 `$CUBRID`·`$CUBRID_DATABASES`가 잡혀 있어야 함.
- CUBRID 버전이 M과 같아야 함.
- `uname -n`이 `slave_host`와 같아야 함(HA 노드 이름은 각 서버의 호스트 이름과 맞아야 함). M에서 NS 이름이, NS에서 M 이름이 풀려야 함.
- 그 DB 서버는 NS에서 꺼져 있어야 함.
- NS의 cub_master가 HA가 아닌 상태로 떠 있으면 task가 내림(`cub_commdb -A`, `cubrid master stop`과 같은 동작). 단, 그때 NS에 다른 DB 서버가 하나라도 떠 있으면 내리지 않고 시작 전에 멈춤(`precheck_failed`, 떠 있는 DB 이름을 알림). 사용자가 그 DB들을 멈춘 뒤 다시 시도해야 함.
- NS에 CMS가 있으면 그대로 둠(`cub_commdb -A`로는 CMS가 내려가지 않음, 실측).

레플리카
- 이전 판과 같음. ssh 접속, 버전이 같을 것, DB가 standby일 것.

## 0. `ha_convert_master` — 싱글 DB를 HA 마스터로 전환 (즉시 응답, 뒤에서 실행)

M DB가 재시작되므로 서비스가 중단됨. 진행 조회·취소는 `ha_make_slave_status`·`ha_make_slave_cancel`을 같이 씀(같은 작업 목록이며 한 번에 하나만 돌아감).

요청

| 키 | 필수 | 뜻 |
|---|---|---|
| `db_name` | 예 | M의 싱글 DB 이름 |
| `confirm_master_restart` | 예 | `yes`여야 시작함(M DB 재시작 확인). 없으면 `failure`, `note`=`confirm_master_restart` |
| `master_host` | 아니오 | M의 노드 이름. 기본 M의 `uname -n`(같아야 함) |
| `ha_group` | 아니오 | HA 그룹 이름. 기본 `cubrid` |
| `ha_port_id` | 아니오 | 기본 59901 |

응답: `uuid`, `job_status`=`running`.

거절하는 경우(시작 전, 바로 응답): DB가 떠 있지 않음, 이미 HA임(`already_ha`), M에 다른 DB 서버가 떠 있음(cub_master를 HA로 다시 띄우면 같이 내려가므로 자동으로 내리지 않음. 떠 있는 DB 이름을 알림), M에 cubrid_ha.conf가 있고 다른 HA 설정이 들어 있음.

단계

| no | id | 하는 일 | 확인 | 되돌리기 |
|---|---|---|---|---|
| 1 | `convert_precheck` | DB가 싱글로 떠 있는지, 다른 DB 서버가 없는지, 노드 이름이 맞는지 | 실패하면 종료 | 없음 |
| 2 | `convert_stop` | **서비스 중단 시작.** `cubrid server stop <db>`(설정을 바꾸기 전에 함. `ha_mode=on`이 된 뒤에는 `cubrid server stop`이 "The server was configured for HA"로 거부함, 실측) | 명령 출력 | `cubrid server start <db>`로 싱글 기동 |
| 3 | `convert_config` | cubrid.conf `ha_mode=on`·`force_remove_log_archives=no`. cubrid_ha.conf를 씀(`ha_port_id`, `ha_node_list=<ha_group>@<master_host>`, `ha_db_list=<db>`, `ha_copy_sync_mode=sync`). databases.txt 그 DB 줄 호스트를 `master_host`로 바꿈. 모두 `.bak.<uuid>` 남김 | 다시 읽어 확인 | `.bak` 복원 |
| 4 | `convert_restart` | cub_master 내림(`cub_commdb -A`), `cubrid heartbeat start` | 명령 출력 | heartbeat 멈춤, cub_master 내림 |
| 5 | `convert_verify` | `cubrid changemode <db>@localhost`가 active가 될 때까지 기다림(최대 180초). **서비스 중단 끝** | active | 없음 |

- 상태 응답에 `downtime_sec`(DB를 멈춘 때부터 active가 된 때까지 걸린 초)를 넣음.
- 실측(11.4.5.1908, 컨테이너, DB 64 MB):
  - `downtime_sec` 14초.
  - 이 동안 M의 CMS는 매초 응답함(재기동 영향 없음).
  - 브로커(broker1)로 1초마다 보낸 SELECT는 2번(약 3초, DB를 멈춘 직후)만 실패함. heartbeat start로 서버가 뜬 뒤에는 active가 되기 전에도 SELECT가 성공함. 쓰기는 따로 재지 않음.
- 브로커 설정은 바꾸지 않았고 브로커를 다시 띄울 필요도 없었음. databases.txt 호스트가 `localhost`에서 `master_host`로 바뀌어도 broker1은 그대로 붙음.
- 실패하면 싱글로 되돌아감(실측: 2단계 실패 → 설정 복원 → `cubrid server start`로 싱글 기동, 약 4초).

## 1. `ha_check_ssh` — ssh 연결 확인 (동기, 최대 약 20초)

이전과 같음. M의 CMS에서 NS와 각 레플리카에 대고 부름.

- 요청: `host`, `ssh_port`, `os_user`, `os_password`(지문 확인 뒤), `hostkey_sha256`.
- 응답: `result`(`ok`/`unreachable`/`hostkey_unknown`/`hostkey_mismatch`/`auth_failed`/`cubrid_not_found`/`version_mismatch`/`error`), `message`, `hostkey_type`, `hostkey_sha256`, `remote_hostname`, `remote_cubrid_rel`, `remote_version`, `local_version`, `remote_cubrid`, `remote_cubrid_databases`.

## 2. `ha_make_slave` — 슬레이브 추가 시작 (즉시 응답, 뒤에서 실행)

요청

| 키 | 필수 | 뜻 |
|---|---|---|
| `db_name` | 예 | M의 DB 이름 |
| `slave_host` | 예 | NS의 노드 이름(= NS `uname -n`). 접속 주소도 이 이름 |
| `ssh_port`, `os_user`, `os_password`, `hostkey_sha256` | 예(포트는 기본 22) | NS용. `ha_check_ssh`와 같음 |
| `replicas` | 아니오 | 레플리카 배열. 원소: `host`, `ssh_port`, `os_user`, `os_password`, `hostkey_sha256`. 마스터의 `ha_replica_list`와 같아야 함 |
| `master_host` | 아니오 | M의 노드 이름. 기본 M의 `uname -n`. M의 `ha_node_list`에 있어야 함 |
| `repl_log_home` | 아니오 | NS의 복제 로그 상위 폴더. 기본 NS `$CUBRID_DATABASES` |
| `master_backup_path` | 아니오 | M에서 백업을 둘 폴더. 기본 M `$CUBRID_DATABASES/ha_make_slave_backup`. 그 아래 uuid 폴더를 만들고 끝나면 지움 |
| `slave_backup_path` | 아니오 | NS에서 받은 백업을 둘 폴더. 기본 NS `$CUBRID_DATABASES/ha_make_slave_backup`. 위와 같음 |
| `start_ha` | 아니오 | `yes`(기본)면 NS HA 시작과 마지막 확인까지 함. `no`면 절차 9·10은 사용자가 함 |

응답

- 시작함: `uuid`, `job_status`=`running`.
- M이 싱글: `failure`, `note`=`master_not_ha`(먼저 `ha_convert_master`를 부르라는 뜻).
- 다른 작업이 진행 중이면: `failure`이고 `note`에 그 uuid가 있음.

## 3. `ha_make_slave_status` — 진행 조회

이전과 같음. `uuid`, `offset`을 받고 `job_status`, `phase`, `step`, `step_no`, `step_count`, `steps[]`(`id`, `no`, `title`, `state`, `detail`), `output`, `next_offset`, `error`, `message`, `exit_code`, `kept_db_paths`, `replica_hosts`, `started_at`, `ended_at`을 돌려줌.
추가 키: `job_kind`(`convert_master`/`make_slave`), `slave_host`, `downtime_sec`(HA 마스터 전환일 때).

- CMS가 재시작된 뒤에는 상태 파일로 조회함. 재시작 때 진행 중이던 작업은 `interrupted`로 보이고 되돌리지 않음.
- `error` 코드는 이전과 같음. NS에서 난 실패는 `message`가 `slave <이름>:`으로, 레플리카에서 난 실패는 `replica <이름>:`으로 시작함.

## 4. `ha_make_slave_cancel` — 취소

이전과 같음.

## 5. `ha_make_slave_cleanup` — NS에 옮겨 보관한 옛 DB 지우기

옛 DB가 이제 NS에 있으므로 NS 비밀번호가 다시 필요함.

- 요청: `uuid`(성공한 작업), `os_password`(NS). 호스트·포트·계정·지문은 상태 파일에 있는 값을 씀.
- 응답: `removed`.

## 단계 (`steps` 순서)

순서는 매뉴얼의 노드별 작업에 맞춤: 새 노드의 HA를 먼저 시작하고, 그 뒤에 기존 노드에 새 노드를 추가함.

| no | id | 어디서 | 하는 일 | 확인 | 되돌리기 |
|---|---|---|---|---|---|
| 1 | `precheck` | M | 입력 검사, 버전·환경, DB가 HA active인지, `ha_node_list`에 `master_host`가 있는지, 레플리카 목록 대조 | 실패하면 종료 | 없음 |
| 2 | `slave_connect` | NS | ssh 접속, 지문, 로그인 | 지문·인증 | 없음 |
| 3 | `slave_check` | NS | `uname -n`=`slave_host`, CUBRID 경로·버전, 그 DB가 꺼져 있는지, cub_master 상태와 다른 DB 서버(HA 아닌 cub_master가 떠 있고 DB 서버도 있으면 멈춤), M 이름이 풀리는지 | 위 조건 | 없음 |
| 4 | `replica_check` | 레플리카마다 | 이전과 같음 | 같음 | 없음 |
| 5 | `slave_config` | NS(SFTP) | cubrid.conf `ha_mode=on`, `force_remove_log_archives=no`. cubrid_ha.conf는 M의 [common]을 옮기고 노드 목록에 NS 추가, `ha_copy_sync_mode`를 노드 수만큼 늘림. databases.txt에 그 DB 줄. `.bak.<uuid>` 남김 | 다시 읽어 확인 | `.bak` 복원 |
| 6 | `master_backup` | M | `cubrid backupdb -C -D <폴더> --no-check <db>@localhost` | 종료 코드 | 백업 폴더 지움 |
| 7 | `send_backup` | M→NS(SFTP) | 백업 파일과 `<db>_bkvinf`를 올림. 끊기면 올린 자리부터 이어 올림 | 파일마다 크기와 sha256(NS에서 계산) | NS 쪽 받은 폴더 지움 |
| 8 | `slave_keep_old_db` | NS | 기존 볼륨·로그·복제 로그 폴더가 있으면 `<경로>.ha_make_slave_<uuid>`로 옮겨 보관 | 옮겨졌는지 | 제자리로 |
| 9 | `slave_restore` | NS | bkvinf를 로그 폴더에 두고 `cubrid restoreslave -s master -m <M> -B <받은 폴더> <db>` | 종료 코드 | 새 볼륨·로그 지움 |
| 10 | `slave_copylog` | NS | `cub_admin copylogdb -L <repl_log_home>/<db>_<M> -m async --start-page-id=-1 <db>@<M>` | 종료 코드 | 복제 로그 폴더 지움 |
| 11 | `slave_ha_start` | NS | HA 아닌 cub_master가 떠 있으면(DB 서버 없음 확인됨) `cub_commdb -A`로 내린 뒤 `cubrid heartbeat start <db>`(`start_ha=no`면 건너뜀) | 명령 출력 | `cubrid heartbeat stop <db>`, cub_master를 작업 전 상태로(없었으면 내림, HA 아닌 상태로 떠 있었으면 다시 띄움) |
| 12 | `master_config` | M | 절차 6·8: cubrid_ha.conf 노드 목록에 NS 추가(+`ha_copy_sync_mode`), databases.txt 호스트에 NS 추가, `cubrid heartbeat reload`. `.bak.<uuid>` 남김 | 다시 읽어 확인, 명령 출력 | `.bak` 복원 후 reload |
| 13 | `replication_start` | M | 절차 7: `cubrid heartbeat replication start <NS>` | 명령 출력 | `replication stop <NS>`(12단계 되돌리기보다 먼저), 새로 생긴 NS 로그 복사 폴더 지움 |
| 14 | `replica_config` | 레플리카마다 | 12단계와 같음 | 같음 | 같음 |
| 15 | `replica_replication_start` | 레플리카마다 | 13단계와 같음 | 같음 | 같음 |
| 16 | `verify` | NS | `cubrid changemode <db>@localhost`가 standby가 될 때까지 최대 120초(`start_ha=no`면 건너뜀) | standby | 없음 |
| 17 | `cleanup` | M·NS | 이번 작업 백업 폴더 지움. 옮겨 보관한 옛 DB는 남김 | 지워졌는지 | 없음 |

- 레플리카가 없으면 4·14·15단계는 "no replicas"로 바로 끝남.
- 13·15단계가 필요한 이유(실측): `heartbeat reload`는 노드 목록만 다시 읽고 프로세스를 띄우지 않음. 그래서 replication start가 없으면 failover 직후 쓴 데이터가 옛 마스터에서 빠짐.
- 되돌리기: 실패하거나 취소되면 바꾼 것이 있는 단계(5~15)만 거꾸로 되돌림. 되돌리기 명령은 취소되지 않음.
- 원격 명령은 `bash -lc`로 돌림. 데몬을 띄우는 명령(heartbeat start, cub_master)은 출력을 NS의 작업 폴더 파일로 받은 뒤 읽음. 데몬이 ssh 채널을 붙잡아 명령이 끝나지 않는 것을 막기 위함임.
- 상태 파일: M의 `$CUBRID/tmp/ha_make_slave/<uuid>/state.json`. 단계 상태, 노드별로 고친 파일과 `.bak`, 옮긴 폴더, NS 접속 정보(비밀번호 제외)를 담음.

## POC 한계

- HA 마스터 전환은 M DB를 재시작함(서비스 중단). 실패해 되돌릴 때 한 번 더 재시작됨.
- 전환 대상 M이나 NS에 다른 DB 서버가 떠 있으면 자동으로 내리지 않고 멈춤. cub_master를 내리면 그 노드의 DB 서버가 모두 내려가기 때문임.
- CMS가 재시작되면 진행 중이던 작업은 `interrupted`로만 보이고 되돌리지 않음.
- 여러 서버의 변경을 한 번에 성공시키거나 한 번에 취소하는 것은 안 됨. 되돌리기는 단계별로 최선을 다할 뿐임.
- 기존 슬레이브를 백업 원본으로 쓰는 방식은 하지 않음. 백업은 항상 M에서 받음.
- 같은 설정 파일을 다른 CMS가 동시에 저장하는 경우의 충돌 방지는 없음.
- 윈도우.
