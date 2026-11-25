# HA 슬레이브 추가 POC — 마스터 CMS 실행형 규격 (2안, v3.1 호스트 단위, 기존 노드 전부)

마스터(M)의 CMS에서 실행해 새 서버(NS)를 슬레이브로 만드는 POC. 슬레이브 추가와 HA 마스터 전환은 DB가 아니라 호스트(노드) 단위로 함. 동작은 둘로 나뉨.

- (1) HA 마스터 전환(`ha_convert_master`): HA가 아닌 M을 노드 1개짜리 HA 마스터로 바꿈. HA로 넣을 DB 목록을 받음(이것이 `ha_db_list`가 됨). 고르지 않은 DB는 싱글로 계속 씀. M의 cub_master가 다시 뜨므로 떠 있던 DB가 모두 재시작됨.
- (2) 슬레이브 추가(`ha_make_slave`): M이 HA 마스터라는 전제에서 NS를 슬레이브로 만듦. 대상은 M의 `ha_db_list`에 있는 DB 전체임. M의 CMS task가 NS와 기존 노드(M 말고 이미 있는 슬레이브와 레플리카)를 원격으로 다룸.

호스트 단위인 이유: cub_master, cubrid_ha.conf, `ha_node_list`는 노드 단위임. 또 M에서 `cubrid heartbeat replication start <NS>`를 하면 `ha_db_list`의 모든 DB에 대해 복제 프로세스(copylogdb·applylogdb)가 뜸. 그래서 DB 하나만 NS에 복원하면 나머지 DB의 복제 프로세스가 실패함.

기존 노드: M의 `ha_node_list`에서 M과 NS를 뺀 노드(기존 슬레이브)와 M의 `ha_replica_list`의 노드(레플리카)임. 매뉴얼의 "서비스 운영 중 슬레이브 하나 더 추가"처럼 기존 노드마다 노드 목록에 NS를 넣고 reload, `replication start <NS>`를 함. 기존 슬레이브는 나중에 active가 될 수 있으므로 NS도 그 슬레이브의 로그를 복사해야 함(NS의 `ha_node_list`에 기존 슬레이브가 들어감).

NCA 위자드는 M이 HA가 아니면 (1)을 부르고 끝나면 (2)를 이어서 부름. (1)만 끝나고 멈춰도 M은 정상 HA 마스터로 남음.

- M 쪽 작업은 로컬에서 함. NS와 기존 노드 쪽 작업은 M의 CMS에 정적으로 링크한 ssh 라이브러리(libssh2)로 함. 시스템 ssh·scp 명령은 쓰지 않음.
- NS에는 CMS가 없어도 됨. NS와 기존 노드의 CMS, 엔진은 바꾸지 않음.
- 스크립트(ha_make_slavedb.sh)는 쓰지 않음.
- 이전 판: DB 하나 단위 마스터 실행형은 976e5f1, 새 슬레이브 CMS 실행형은 a641627.

POC 조건: Rocky 8, CUBRID 11.4.5.1908. 시험 구성은 (1) `ha_db_list`에 DB 2개인 HA 마스터 M + 레플리카 R에 NS 추가, (2) DB 2개인 싱글 M에서 그중 하나만 HA로 바꾸고 NS 추가, (3) M + 기존 슬레이브 S1에 NS 추가. 각 서버의 OS 계정 아이디·비밀번호만 주어짐.

## 공통

- 모두 admin 전용(AU_ADMIN). 요청·응답은 CMS JSON 그대로(`task`, `token`, 응답 `status`·`note`).
- 비밀번호 키 이름은 `os_password`(NCA 로그 가림 규칙: 키에 `pass` 포함). 기존 노드 원소 안에서도 같은 이름.
- 비밀번호는 M의 CMS 메모리에만 두고 작업이 끝나면 지움. 명령줄 인자·로그·상태 파일·비동기 요청 사본에 남기지 않음. 이 task들은 CMS의 기존 async 틀을 쓰지 않음.
- 호스트 키 지문 형식은 OpenSSH와 같음: `SHA256:` + base64(패딩 없음). libssh2가 고르는 키는 보통 ecdsa임.
- 숫자 값(`ssh_port`, `offset`)은 문자열과 숫자 둘 다 받음.
- DB 목록 값(`db_names`)은 배열 또는 쉼표로 구분한 문자열을 받음. 응답의 DB 목록은 항상 배열임.
- 빌드: libssh2 1.11.1과 OpenSSL 3.5.7을 ExternalProject로 소스에서 정적으로 빌드함(sha256 고정). 리눅스 빌드만 해당함.

## ha_mode=on 노드의 싱글 DB (실측·소스 확인)

- cubrid.conf에 `[@<db>]` 절을 두고 `ha_mode=off`를 쓰면, `[common]`이 `ha_mode=on`이고 heartbeat가 도는 노드에서도 그 DB를 `cubrid server start <db>`로 싱글로 띄울 수 있음.
- 엔진은 DB별 설정을 읽을 때 `[common]` 다음에 `[@<db>]` 절을 읽음. 그 결과 `ha_mode`가 off면 cubrid_ha.conf를 읽지 않음. `cubrid server start|stop`은 이 DB별 값으로 HA 여부를 판단함. cub_master는 HA 서버와 HA 아닌 서버를 같이 관리함.
- 실측(11.4.5.1908, heartbeat가 도는 HA 마스터): `[@cubdb] ha_mode=off`를 넣은 뒤 `cubrid server start cubdb` 성공, 접속·조회 정상, `cubrid heartbeat reload` 뒤에도 그대로 뜸, `cubrid server stop cubdb` 성공. `cubrid changemode cubdb`는 "not configured for HA"임. 같은 노드의 HA DB(hadb)는 영향 없음.
- `[@<db>]` 절이 없으면 `cubrid server start|stop <db>`는 "The server was configured for HA"로 거부함.
- 단, cub_master를 내리면(`cub_commdb -A`) 그 노드의 DB 서버가 HA 여부와 상관없이 모두 내려감.

## 사전 조건

M(이 CMS가 도는 서버)
- 슬레이브 추가: `[common]`이 `ha_mode=on`이고, cubrid_ha.conf `ha_db_list`의 DB가 모두 M에서 active여야 함(`cubrid changemode <db>@localhost`). HA가 아니면 `master_not_ha`로 거절하고, 먼저 HA 마스터 전환을 하라고 알림.
- HA 마스터 전환: `[common]`이 `ha_mode=on`이 아니고 cubrid_ha.conf에 `ha_node_list`가 없어야 함. HA로 넣을 DB는 databases.txt에 있어야 하고, cubrid.conf의 그 DB 절(`[@<db>]`)에 `ha_mode`가 따로 적혀 있으면 안 됨.
- `master_host`(M의 노드 이름)는 M의 `uname -n`과 같아야 하고, NS와 기존 노드에서 그 이름이 풀려야 함.
- 기존 노드(M의 `ha_node_list`의 다른 노드, `ha_replica_list`의 노드)는 모두 `existing_nodes`로 넘겨야 함. 빠지거나 두 목록에 없는 노드가 있으면 시작 전에 거절함.
- 마스터 보관 로그 보존(`log_max_archives` 등)은 사용자가 미리 맞춰 둠. DB가 여럿이면 첫 DB의 백업부터 NS HA 시작까지 시간이 길어지므로 DB 전체 백업·전송 시간만큼 보관 로그가 남아 있어야 함.

NS(새 슬레이브)
- sshd가 떠 있고, 입력한 OS 계정이 CUBRID 운영 계정이어야 함. 로그인 셸에 `$CUBRID`·`$CUBRID_DATABASES`가 잡혀 있어야 함.
- CUBRID 버전이 M과 같아야 함.
- `uname -n`이 `slave_host`와 같아야 함(HA 노드 이름은 각 서버의 호스트 이름과 맞아야 함). M에서 NS 이름이, NS에서 M 이름이 풀려야 함.
- `ha_db_list`의 DB가 NS에 떠 있으면 안 됨.
- NS의 cub_master가 HA가 아닌 상태로 떠 있으면 task가 내림(`cub_commdb -A`). 단, 그때 NS에 DB 서버가 하나라도 떠 있으면 내리지 않고 시작 전에 멈춤(`other_db_running`, 떠 있는 DB 이름을 알림).
- `ha_db_list`의 DB가 NS에 이미 있으면(볼륨·로그 폴더) DB마다 옮겨 보관함.
- NS에 CMS가 있으면 그대로 둠(`cub_commdb -A`로는 CMS가 내려가지 않음, 실측).

기존 노드(슬레이브·레플리카)
- ssh 접속, 버전이 같을 것, `ha_db_list`의 DB가 모두 standby일 것, 그 노드의 `ha_db_list`에 그 DB가 모두 있고 `ha_node_list`에 M이 있을 것.

## 0. `ha_convert_master` — HA 마스터로 전환 (즉시 응답, 뒤에서 실행)

M의 cub_master가 다시 뜨므로 M의 DB가 모두 멈췄다 다시 뜸(서비스 중단). 진행 조회·취소는 `ha_make_slave_status`·`ha_make_slave_cancel`을 같이 씀(같은 작업 목록이며 한 번에 하나만 돌아감).

요청

| 키 | 필수 | 뜻 |
|---|---|---|
| `db_names` | 예 | HA로 넣을 DB 목록(= 새 `ha_db_list`). 떠 있지 않은 DB도 됨(heartbeat가 띄움) |
| `confirm_master_restart` | 예 | `yes`여야 시작함(HA로 넣는 DB 재시작 확인) |
| `confirm_other_db_restart` | 다른 DB가 떠 있을 때 | `yes`여야 시작함(고르지 않았지만 떠 있는 DB도 같이 재시작됨 확인) |
| `master_host` | 아니오 | M의 노드 이름. 기본 M의 `uname -n`(같아야 함) |
| `ha_group` | 아니오 | HA 그룹 이름. 기본 `cubrid` |
| `ha_port_id` | 아니오 | 기본 59901 |

`db_name`(DB 하나)은 더 받지 않음. 보내면 `invalid db_name (not used: pass db_names)`로 거절함(이전 판 화면이 잘못 부르는 것을 막기 위함).

확인 응답(시작 전, 바로 응답)
- 확인 키가 모자라면 `failure`이고 `note`가 `confirm_master_restart` 또는 `confirm_other_db_restart`임. 이때 응답에 다음을 넣음. 화면은 이 목록을 보여 주고 확인을 받은 뒤 같은 요청에 확인 키를 더해 다시 부름.
  - `ha_databases`: HA로 넣을 DB(요청 그대로)
  - `restart_databases`: 지금 떠 있어서 멈췄다 다시 뜰 DB 전체
  - `other_running_databases`: 그중 고르지 않은 DB(싱글로 다시 뜸)
  - `single_databases`: databases.txt에 있으나 고르지 않은 DB 전체(전환 뒤에도 싱글로 씀. cubrid.conf에 `[@<db>] ha_mode=off`가 들어감)
  - `message`: 설명 한 줄

거절하는 경우(시작 전, 바로 응답): 이미 HA임(`already_ha`), cubrid_ha.conf에 `ha_node_list`가 이미 있음, `db_names`의 DB가 databases.txt에 없음, 그 DB 절에 `ha_mode`가 따로 적혀 있음.

단계

| no | id | 하는 일 | 확인 | 되돌리기 |
|---|---|---|---|---|
| 1 | `convert_precheck` | 위 조건, 노드 이름, 떠 있는 DB 목록 | 실패하면 종료 | 없음 |
| 2 | `convert_stop` | **서비스 중단 시작.** 떠 있는 DB를 모두 `cubrid server stop <db>`로 멈춤(설정을 바꾸기 전에 함. `ha_mode=on`이 된 뒤에는 HA로 넣을 DB를 `cubrid server stop`으로 멈출 수 없음) | 명령 출력 | 처음에 떠 있던 DB를 모두 `cubrid server start <db>`로 싱글 기동 |
| 3 | `convert_config` | cubrid.conf `[common]` `ha_mode=on`·`force_remove_log_archives=no`, 고르지 않은 DB마다 `[@<db>]` `ha_mode=off`. cubrid_ha.conf를 씀(`ha_port_id`, `ha_node_list=<ha_group>@<master_host>`, `ha_db_list=<db_names 쉼표 구분>`, `ha_copy_sync_mode=sync`). databases.txt의 HA DB 줄 호스트를 `master_host`로 바꿈. 모두 `.bak.<uuid>` 남김 | 다시 읽어 확인 | `.bak` 복원 |
| 4 | `convert_restart` | cub_master 내림(`cub_commdb -A`), `cubrid heartbeat start`(DB 이름 없이. `ha_db_list` 전체를 띄움) | 명령 출력 | `cubrid heartbeat stop`, cub_master 내림 |
| 5 | `convert_verify` | HA DB마다 `cubrid changemode <db>@localhost`가 active가 될 때까지 기다림(최대 180초). **HA DB 서비스 중단 끝** | 모두 active | 없음 |
| 6 | `convert_start_others` | 2단계에서 멈춘 DB 중 고르지 않은 DB를 `cubrid server start <db>`로 싱글 기동. 없으면 "no other databases" | 명령 출력 | 그 DB를 `cubrid server stop` |

- 상태 응답에 `downtime_sec`(2단계 시작부터 HA DB가 모두 active가 된 때까지 걸린 초)를 넣음.
- 실측(DB 1개일 때, 11.4.5.1908, 컨테이너, DB 64 MB): `downtime_sec` 14초, 이 동안 M의 CMS는 매초 응답함, 브로커로 1초마다 보낸 SELECT는 2번(약 3초)만 실패함. 브로커 설정은 바꾸지 않았고 다시 띄울 필요도 없었음.
- 실패하면 싱글로 되돌아감: 설정 복원, heartbeat·cub_master 내림, 처음에 떠 있던 DB를 모두 싱글로 띄움.

## 1. `ha_check_ssh` — ssh 연결 확인 (동기, 최대 약 20초)

이전과 같음. M의 CMS에서 NS와 각 기존 노드에 대고 부름.

- 요청: `host`, `ssh_port`, `os_user`, `os_password`(지문 확인 뒤), `hostkey_sha256`.
- 응답: `result`(`ok`/`unreachable`/`hostkey_unknown`/`hostkey_mismatch`/`auth_failed`/`cubrid_not_found`/`version_mismatch`/`error`), `message`, `hostkey_type`, `hostkey_sha256`, `remote_hostname`, `remote_cubrid_rel`, `remote_version`, `local_version`, `remote_cubrid`, `remote_cubrid_databases`.

## 2. `ha_make_slave` — 슬레이브 추가 시작 (즉시 응답, 뒤에서 실행)

대상 DB는 M의 cubrid_ha.conf `ha_db_list` 전체임. `db_name`은 받지 않음(보내면 `invalid db_name (not used: every database in ha_db_list)`로 거절).

요청

| 키 | 필수 | 뜻 |
|---|---|---|
| `slave_host` | 예 | NS의 노드 이름(= NS `uname -n`). 접속 주소도 이 이름 |
| `ssh_port`, `os_user`, `os_password`, `hostkey_sha256` | 예(포트는 기본 22) | NS용. `ha_check_ssh`와 같음 |
| `existing_nodes` | 기존 노드가 있으면 예 | 기존 노드 배열. 원소: `host`, `ssh_port`, `os_user`, `os_password`, `hostkey_sha256`. 역할(슬레이브·레플리카)은 M의 설정으로 정함. M의 `ha_node_list`(M·NS 제외)와 `ha_replica_list`를 합친 것과 같아야 함 |
| `replicas` | 아니오 | 이전 판 이름. `existing_nodes`와 같게 받음(둘 다 보내면 합침) |
| `master_host` | 아니오 | M의 노드 이름. 기본 M의 `uname -n`. M의 `ha_node_list`에 있어야 함 |
| `repl_log_home` | 아니오 | NS의 복제 로그 상위 폴더. 기본 NS `$CUBRID_DATABASES` |
| `master_backup_path` | 아니오 | M에서 백업을 둘 폴더. 기본 M `$CUBRID_DATABASES/ha_make_slave_backup`. 그 아래 `<uuid>/<db>` 폴더를 만들고 끝나면 지움 |
| `slave_backup_path` | 아니오 | NS에서 받은 백업을 둘 폴더. 기본 NS `$CUBRID_DATABASES/ha_make_slave_backup`. 위와 같음 |
| `start_ha` | 아니오 | `yes`(기본)면 NS HA 시작과 마지막 확인까지 함. `no`면 HA 시작·확인은 사용자가 함 |

응답

- 시작함: `uuid`, `job_status`=`running`, `db_names`(대상 DB 배열, M의 `ha_db_list` 순서).
- M이 HA가 아님: `failure`, `note`=`master_not_ha`(먼저 `ha_convert_master`를 부르라는 뜻).
- 다른 작업이 진행 중이면: `failure`이고 `note`에 그 uuid가 있음.

## 3. `ha_make_slave_status` — 진행 조회

`uuid`, `offset`을 받고 다음을 돌려줌: `job_status`, `job_kind`(`convert_master`/`make_slave`), `phase`, `step`, `step_no`, `step_count`, `step_db`, `steps[]`(`id`, `no`, `title`, `db`, `state`, `detail`), `db_names`, `output`, `next_offset`, `error`, `message`, `exit_code`, `kept_db_paths`, `master_host`, `slave_host`, `existing_nodes`(`host`, `role`=`slave`/`replica`), `started_at`, `ended_at`, `downtime_sec`(HA 마스터 전환일 때).

- DB마다 반복하는 단계는 `steps[]`에 DB 수만큼 들어가고 원소의 `db`에 DB 이름이 있음. 노드 단위 단계는 `db`가 빈 문자열임. 그래서 `step_count`는 DB 수에 따라 달라짐(슬레이브 추가는 12 + 5 × DB 수).
- `step_db`는 지금 단계의 DB 이름(노드 단위 단계면 빈 문자열). 화면은 "단계 `step_no` / `step_count`, `title` (`step_db`)"로 보여 주면 됨.
- `db_names`: 슬레이브 추가는 대상 DB(M의 `ha_db_list`), HA 마스터 전환은 HA로 넣는 DB.
- 이전 판의 `db_name`은 없앰.
- CMS가 재시작된 뒤에는 상태 파일로 조회함. 재시작 때 진행 중이던 작업은 `interrupted`로 보이고 되돌리지 않음.
- 이전 판의 `replica_hosts`는 `existing_nodes`로 바뀜.
- NS에서 난 실패는 `message`가 `new slave <이름>:`으로, 기존 노드에서 난 실패는 `slave <이름>:` 또는 `replica <이름>:`으로 시작함. DB마다 하는 단계의 실패는 그 안에 DB 이름이 들어감.

## 4. `ha_make_slave_cancel` — 취소

이전과 같음.

## 5. `ha_make_slave_cleanup` — NS에 옮겨 보관한 옛 DB 지우기

옛 DB가 이제 NS에 있으므로 NS 비밀번호가 다시 필요함.

- 요청: `uuid`(성공한 작업), `os_password`(NS). 호스트·포트·계정·지문은 상태 파일에 있는 값을 씀.
- 응답: `removed`(DB 전체에서 지운 폴더 목록).

## 슬레이브 추가 단계 (`steps` 순서)

순서는 매뉴얼의 노드별 작업에 맞춤: 새 노드의 HA를 먼저 시작하고, 그 뒤에 기존 노드에 새 노드를 추가함. 6~10단계는 DB마다 반복함(DB 하나의 다섯 단계를 끝낸 뒤 다음 DB로 감). 아래 번호는 DB 1개 기준이며, DB가 N개면 11단계 이후 번호가 5 × (N − 1)만큼 밀림.

| no | id | 단위 | 어디서 | 하는 일 | 확인 | 되돌리기 |
|---|---|---|---|---|---|---|
| 1 | `precheck` | 노드 | M | 입력 검사, 버전·환경, `ha_db_list`의 DB가 모두 HA active인지, `ha_node_list`에 `master_host`가 있는지, 기존 노드 목록 대조(역할 정함) | 실패하면 종료 | 없음 |
| 2 | `slave_connect` | 노드 | NS | ssh 접속, 지문, 로그인 | 지문·인증 | 없음 |
| 3 | `slave_check` | 노드 | NS | `uname -n`=`slave_host`, CUBRID 경로·버전, 대상 DB가 모두 꺼져 있는지, cub_master 상태와 다른 DB 서버, M 이름이 풀리는지 | 위 조건 | 없음 |
| 4 | `existing_check` | 노드 | 기존 노드마다 | 접속·버전, 대상 DB가 모두 standby인지, 그 노드의 `ha_db_list`·`ha_node_list` | 같음 | 없음 |
| 5 | `slave_config` | 노드 | NS(SFTP) | cubrid.conf `ha_mode=on`, `force_remove_log_archives=no`. cubrid_ha.conf는 M의 [common]을 옮기고 노드 목록에 NS 추가, `ha_db_list`는 M과 같게, `ha_copy_sync_mode`를 노드 수만큼 늘림. databases.txt에 대상 DB 줄 전부. `.bak.<uuid>` 남김 | 다시 읽어 확인 | `.bak` 복원 |
| 6 | `master_backup` | DB | M | `cubrid backupdb -C -D <폴더>/<db> --no-check <db>@localhost` | 종료 코드 | 그 DB 백업 폴더 지움 |
| 7 | `send_backup` | DB | M→NS(SFTP) | 그 DB 백업 파일과 `<db>_bkvinf`를 올림. 끊기면 다시 올림 | 파일마다 크기와 sha256(NS에서 계산) | NS 쪽 그 DB 받은 폴더 지움 |
| 8 | `slave_keep_old_db` | DB | NS | 그 DB의 기존 볼륨·로그·복제 로그 폴더가 있으면 `<경로>.ha_make_slave_<uuid>`로 옮겨 보관 | 옮겨졌는지 | 제자리로 |
| 9 | `slave_restore` | DB | NS | bkvinf를 로그 폴더에 두고 `cubrid restoreslave -s master -m <M> -B <받은 폴더> <db>` | 종료 코드 | 새 볼륨·로그 지움 |
| 10 | `slave_copylog` | DB | NS | `cub_admin copylogdb -L <repl_log_home>/<db>_<M> -m async --start-page-id=-1 <db>@<M>` | 종료 코드 | 복제 로그 폴더 지움 |
| 11 | `slave_ha_start` | 노드 | NS | HA 아닌 cub_master가 떠 있으면(DB 서버 없음 확인됨) `cub_commdb -A`로 내린 뒤 `cubrid heartbeat start`(DB 이름 없이. NS의 `ha_db_list` = 대상 DB 전체를 띄움, 하나라도 실패하면 엔진이 그 노드 HA를 다시 내림)(`start_ha=no`면 건너뜀) | 명령 출력 | `cubrid heartbeat stop`, cub_master를 작업 전 상태로(없었으면 내림, HA 아닌 상태로 떠 있었으면 다시 띄움) |
| 12 | `master_config` | 노드 | M | cubrid_ha.conf 노드 목록에 NS 추가(+`ha_copy_sync_mode`), databases.txt의 대상 DB 줄 호스트에 NS 추가, `cubrid heartbeat reload`. `.bak.<uuid>` 남김 | 다시 읽어 확인, 명령 출력 | `.bak` 복원 후 reload |
| 13 | `replication_start` | 노드 | M | `cubrid heartbeat replication start <NS>`(대상 DB 전체에 대해 copylogdb·applylogdb가 뜸) | 명령 출력 | `replication stop <NS>`(12단계 되돌리기보다 먼저), 새로 생긴 DB별 NS 로그 복사 폴더 지움 |
| 14 | `existing_config` | 노드 | 기존 노드마다 | 12단계와 같음 | 같음 | 같음 |
| 15 | `existing_replication_start` | 노드 | 기존 노드마다 | 13단계와 같음 | 같음 | 같음 |
| 16 | `verify` | 노드 | NS | 대상 DB마다 `cubrid changemode <db>@localhost`가 standby가 될 때까지, 그리고 NS의 `cubrid heartbeat status`에 DB마다 M과 기존 슬레이브 각각의 로그 복사(Copylogdb `<db>@<노드>`)가 보일 때까지 최대 120초(`start_ha=no`면 건너뜀) | 모두 standby, 로그 복사 모두 있음 | 없음 |
| 17 | `cleanup` | 노드 | M·NS | 이번 작업 백업 폴더 지움. 옮겨 보관한 옛 DB는 남김 | 지워졌는지 | 없음 |

- 기존 노드가 없으면 4·14·15단계는 "no existing slaves or replicas"로 바로 끝남.
- 이전 판의 단계 id `replica_check`·`replica_config`·`replica_replication_start`는 `existing_check`·`existing_config`·`existing_replication_start`로 바뀜.
- 13·15단계가 필요한 이유(실측): `heartbeat reload`는 노드 목록만 다시 읽고 프로세스를 띄우지 않음. 그래서 replication start가 없으면 failover 직후 쓴 데이터가 옛 마스터에서 빠짐.
- 되돌리기: 실패하거나 취소되면 바꾼 것이 있는 단계만 거꾸로 되돌림. DB마다 하는 단계도 DB별로 거꾸로 되돌림(예: 두 번째 DB의 복원에서 실패하면 두 번째 DB의 8·7·6단계, 첫 번째 DB의 10~6단계, 5단계 순서). 되돌리기 명령은 취소되지 않음.
- 원격 명령은 `bash -lc`로 돌림. 데몬을 띄우는 명령(heartbeat start, cub_master)은 출력을 NS의 임시 파일로 받은 뒤 읽음. 데몬이 ssh 채널을 붙잡아 명령이 끝나지 않는 것을 막기 위함임.
- 상태 파일: M의 `$CUBRID/tmp/ha_make_slave/<uuid>/state.json`. 단계 상태, 노드별로 고친 파일과 `.bak`, 옮긴 폴더, NS 접속 정보(비밀번호 제외)를 담음.

## POC 한계

- HA 마스터 전환은 M의 cub_master를 다시 띄우므로 M의 떠 있던 DB가 모두 재시작됨(서비스 중단). 고르지 않은 DB도 잠깐 멈춤. 실패해 되돌릴 때 한 번 더 재시작됨.
- NS에 다른 DB 서버가 떠 있으면 자동으로 내리지 않고 멈춤. cub_master를 내리면 그 노드의 DB 서버가 모두 내려가기 때문임.
- `ha_db_list`의 일부 DB만 NS에 만드는 것은 안 됨(호스트 단위).
- DB를 하나씩 백업·전송하므로 M과 NS에 한 번에 DB 하나 크기만큼이 아니라 DB 전체 백업 크기만큼 자리가 있어야 함(백업은 마지막 단계에서 지움).
- CMS가 재시작되면 진행 중이던 작업은 `interrupted`로만 보이고 되돌리지 않음.
- 여러 서버의 변경을 한 번에 성공시키거나 한 번에 취소하는 것은 안 됨. 되돌리기는 단계별로 최선을 다할 뿐임.
- 기존 슬레이브를 백업 원본으로 쓰는 방식은 하지 않음. 백업은 항상 M에서 받음.
- 같은 설정 파일을 다른 CMS가 동시에 저장하는 경우의 충돌 방지는 없음.
- 윈도우.
