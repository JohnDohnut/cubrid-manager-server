# HA 노드 추가 — 스크립트 + 전용 CMS API 설계 (v4)

싱글 DB를 HA로 바꾸고, HA에 슬레이브·레플리카를 여러 대 한 번에 추가하는 기능의 설계임. 동작은 셸 스크립트 하나에 넣고, CMS에는 그 스크립트만 부르는 얇은 전용 API를 둠. runscript는 쓰지 않음.

## 0. 범위와 전제

- 고객사 배포 환경을 기준으로 함. 구성은 마스터:슬레이브:레플리카 = 1:N:M(N, M ≥ 0)임. 시험용 1:1:1 구성을 기준으로 하지 않음.
- 각 서버에 대해 주어지는 것은 OS 계정 아이디·비밀번호뿐임(ssh 키 배포 없음). 원격 작업은 ssh로 함(폐쇄망 방안은 따로 다룸).
- 한 번 실행으로 새 슬레이브 a대와 새 레플리카 b대(a + b ≥ 1)를 같이 추가함.
- 스크립트는 파일 안의 변수를 고쳐 쓰는 방식(예전 ha_make_slavedb.sh 방식)을 쓰지 않음. 필요한 값은 모두 인자로 받음.

| 케이스 | 실행 위치(L) | 시작 상태 | 하는 일 |
|---|---|---|---|
| 1 | 마스터가 될 서버 | 싱글 DB | 싱글 DB → HA 마스터 승격 + 새 슬레이브 추가 |
| 2 | 마스터가 될 서버 | 싱글 DB | 싱글 DB → HA 마스터 승격 + 새 레플리카 추가 |
| 3 | 마스터 | HA 1:N:M | 새 슬레이브 추가 |
| 4 | 마스터 | HA 1:N:M | 새 레플리카 추가 |
| 5(선택) | 기존 슬레이브 | HA 1:N:M (N ≥ 1) | 그 슬레이브를 백업 원본으로 써서 새 슬레이브·레플리카 추가. 마스터는 원격 |
| 6(선택) | 기존 레플리카 | HA 1:N:M (M ≥ 1) | 그 레플리카를 백업 원본으로 써서 새 슬레이브·레플리카 추가. 마스터는 원격(가능 여부·제약만 적음, 8절) |

- 승격: 싱글 DB를 노드 1개짜리 HA 마스터로 바꾸는 단계 묶음(6.2의 B)임. 요청한 DB가 싱글일 때(케이스 1·2)만 있고, 이미 HA인 케이스(3~6)에는 이 묶음이 아예 없음. 단계 id는 `convert_*`이고 요청 키도 `confirm_master_restart`·`rollback_convert` 그대로이나, 뜻은 승격임.
- 1·2와 3·4는 같은 흐름임. 새 노드 목록에 슬레이브와 레플리카가 섞여 있어도 됨.
- 실행 대상(위자드를 여는 호스트)의 상태로 케이스가 정해짐: 싱글 DB → 1·2, HA 마스터 → 3·4, HA 슬레이브 → 5, HA 레플리카 → 6. 5·6은 선택 구현이며 같은 우선순위로 나중에 함.
- 기호: L = 스크립트가 도는 노드, M = 현재 마스터, S1..SN = 기존 슬레이브, R1..RM = 기존 레플리카, E = 기존 노드 전부(M, S, R. 케이스 5·6은 L도 포함), X1..Xa = 새 슬레이브, Y1..Yb = 새 레플리카, D1..Dk = 대상 DB(`ha_db_list`).

## 1. 큰 그림

### 1.1 구성 요소와 맡는 일

| 구성 요소 | 맡는 일 | 맡지 않는 일 |
|---|---|---|
| CMS 전용 API | 입력 형식 검사, 작업 폴더 만들기, ssh 헬퍼(에이전트)를 띄워 비밀번호를 넘기기, 스크립트 실행, 진행 상태·로그 읽어 주기, 취소 신호 | HA 판단, 설정 편집, 원격 명령, 비밀번호를 스크립트에 넘기기 |
| 작업 스크립트 `ha_add_nodes.sh` | 확인, 승격, 설정 편집, 백업·전송·복원, HA 시작, 기존 노드 등록, 확인, 되돌리기, 진행 기록 | 비밀번호 다루기(아예 모름) |
| ssh 헬퍼 `cm_ssh` | 노드별 접속 정보·비밀번호를 메모리에만 들고 있다가, 스크립트의 요청을 받아 원격 명령 실행·파일 전송을 대신 함(에이전트 모드). 호스트 키 고정 | 판단·흐름 |

- **CMS에는 HA 단계가 없음.** CMS가 하는 일은 작업을 시작하기 전의 준비(C1~C4)와 진행 중의 조회·취소(C5·C6)뿐임. HA 작업 단계는 모두 스크립트가 함. 그래서 CMS와 스크립트가 서로 끝나기를 기다리는 지점은 시작할 때 한 곳(헬퍼 준비)뿐임(7절).
- 스크립트와 헬퍼는 CMS와 분리된 세션(setsid)으로 돎. CMS가 재시작돼도 작업은 끝까지 가고, 재시작한 CMS는 작업 폴더의 파일로 상태를 읽음.
- 스크립트는 CMS 없이 터미널에서도 돌릴 수 있음. 이때 헬퍼가 터미널에서 노드별 비밀번호를 물어봄.

### 1.2 흐름

1. 화면이 `ha_add_nodes`(action=`plan`)를 부름. 스크립트가 모든 노드에 접속해 확인하고, 노드 역할·바뀔 목록·재시작될 DB·단계 표를 돌려줌. 아무것도 바꾸지 않음.
2. 화면이 결과를 보여 주고 확인을 받음.
3. 화면이 `ha_add_nodes`(action=`run`, 확인 키 포함)를 부름. 스크립트가 뒤에서 돌고, 화면은 `ha_add_nodes_status`로 진행을 봄.
4. 실패·취소 시 스크립트가 바꾼 것을 거꾸로 되돌림.

## 2. 노드 역할별 설정 규칙 (매뉴얼·소스로 확인)

### 2.1 슬레이브와 레플리카의 차이

| 항목 | 새 슬레이브 X | 새 레플리카 Y |
|---|---|---|
| cubrid.conf `ha_mode` | `on` | `replica` |
| `force_remove_log_archives` | `no`로 씀 | 엔진이 레플리카에서 `yes`로 고정함(쓰지 않음) |
| 들어갈 목록 | 모든 노드의 `ha_node_list` | 모든 노드의 `ha_replica_list` |
| 자신의 cubrid_ha.conf | M의 `ha_node_list`를 순서 그대로 옮기고 끝에 X1..Xa를 붙임. `ha_replica_list`도 M의 것을 그대로 옮기고 끝에 Y1..Yb를 붙임 | 같음. 자기 이름이 `ha_replica_list`에 있어야 heartbeat가 뜸. 두 목록의 그룹 이름이 같아야 함 |
| `ha_copy_sync_mode` | 값 개수 = `ha_node_list` 노드 수. X가 늘 때마다 마스터·슬레이브 노드에서 값 하나씩 늘림 | 레플리카 노드는 값과 상관없이 항상 async. 레플리카는 `ha_node_list`에 없으므로 다른 노드의 값 개수도 그대로 |
| 이 노드가 로그를 받는 곳 | `ha_node_list`의 다른 노드 전부 | `ha_node_list`의 노드 전부(다른 레플리카에서는 받지 않음) |
| 이 노드의 로그를 받는 곳 | 다른 마스터·슬레이브 전부, 레플리카 전부 | 없음 |
| 기존 노드에서 할 일 | 설정 + reload + `heartbeat repl start <X>` | 설정 + reload만. `repl start <Y>`는 엔진이 거부함(`ha_node_list`에 없는 이름은 "not listed") |
| 복원 | `restoreslave -s <백업 원본의 상태> -m <M>` | 같음 |
| 첫 로그 복사 | M에서 `copylogdb --start-page-id=-1` | 같음 |

- 목록 순서: heartbeat의 우선순위는 각 노드 파일에 적힌 `ha_node_list` 순서임. failover를 겪은 HA에서는 지금 마스터가 맨 앞이 아닐 수 있음. 그래서 새 노드는 M의 목록(`ha_node_list`, `ha_replica_list`, `ha_copy_sync_mode`)을 순서 그대로 옮기고 끝에만 붙임. 기존 노드도 끝에만 붙임. S2에서 기존 노드들의 두 목록이 서로 같은지(순서 포함) 확인하고, 다르면 거절함.
- 근거: 엔진이 copylogdb·applylogdb를 띄우는 대상은 `ha_node_list`의 노드뿐임. 매뉴얼의 "운영 중 레플리카 추가" 절차도 기존 노드는 `ha_replica_list`에 넣고 reload만 하며, 레플리카용 복제 프로세스는 띄우지 말라고 함.
- reload는 노드 목록만 다시 읽고 프로세스를 띄우지 않음. 그래서 새 슬레이브는 기존 노드마다 `repl start`가 필요함(이전 POC 실측: 이게 없으면 failover 직후 쓴 데이터가 옛 마스터에서 빠짐).

### 2.2 기존 노드에서 할 일 (1:N:M)

| 기존 노드 | cubrid_ha.conf | databases.txt | reload | repl start |
|---|---|---|---|---|
| M, S1..SN | `ha_node_list` 끝에 X1..Xa, `ha_copy_sync_mode` 끝에 값 a개, `ha_replica_list` 끝에 Y1..Yb | D마다 db-host += X1..Xa | 1회 | X마다(a회) |
| R1..RM | 같음(`ha_copy_sync_mode`는 무시되지만 개수는 맞춰 둠) | 같음 | 1회 | X마다(a회). 레플리카도 슬레이브의 로그를 받음 |

- a = 0(레플리카만 추가)이면 기존 노드는 `ha_replica_list`만 바뀌고 repl start가 없음.
- N = 0, M = 0이면 기존 노드는 M 하나뿐임.
- 기존 노드 입력 = M 설정의 `ha_node_list`(M과 새 노드 제외) + `ha_replica_list`. 하나라도 빠지거나 목록에 없는 노드가 들어오면 시작 전에 거절함. 역할은 입력으로 받지 않고 M의 설정에서 정함.

### 2.3 새 노드끼리

- 새 노드의 설정에는 처음부터 최종 목록(새 노드 전부 포함)을 씀. 새 노드의 heartbeat가 뜰 때 새 노드끼리의 로그 복사도 같이 뜸.
- 새 노드를 띄우는 순서: X1..Xa 먼저, 그다음 Y1..Yb. Y는 X의 로그도 받으므로 X가 먼저 떠 있어야 함.
- a ≥ 2일 때 확인할 것(실측): Xi의 heartbeat가 뜰 때 아직 뜨지 않은 Xj를 대상으로 한 copylogdb가 등록되고, Xj가 뜨면 따라붙는지. 안 되면 `peer_repl_start` 단계에서 빠진 것만 `repl start <Xj>`로 띄움. 빠진 것이 없으면 아무것도 하지 않는 단계로 둠.

### 2.4 ha_mode=on 노드의 싱글 DB (케이스 1·2)

- cubrid.conf `[@<db>]` 절에 `ha_mode=off`를 쓰면 HA 노드에서도 그 DB를 `cubrid server start`로 싱글로 띄울 수 있음(소스 확인, 11.4.5.1908 실측).
- 승격 때 HA로 넣지 않는 DB는 이 방법으로 싱글로 남김. cub_master를 다시 띄우면 그 노드의 DB 서버가 모두 내려가므로, 떠 있던 DB는 승격 중에 멈췄다 다시 뜸(확인을 받음).

### 2.5 노드마다 다른 경로 (계정·홈이 다른 노드)

- 엔진은 노드마다 경로가 달라도 됨: databases.txt와 `ha_copy_log_base`는 노드마다 그 노드 것을 읽고, restoreslave가 복제 적용 정보에 넣는 복제 로그 경로도 복원하는 노드의 설정으로 계산함(소스). 매뉴얼의 "경로를 같게"는 restoredb를 `-u` 없이 쓰면 백업에 적힌 경로로 복원되기 때문임.
- 규칙

| 값 | 새 노드에서 쓰는 값 |
|---|---|
| databases.txt 볼륨·로그 경로 | M의 경로가 M의 `$CUBRID_DATABASES` 아래면 그 노드의 `$CUBRID_DATABASES` 아래로 바꿈. 아니면(따로 지정한 절대 경로) 그대로 쓰되, S3에서 쓸 수 없으면 거절 |
| lob 경로(`file:…`) | 같은 규칙 |
| bkvinf를 두는 곳 | 그 노드의 로그 경로 |
| 복원 | `restoreslave -u`(그 노드 databases.txt 경로로) |
| 복제 로그 폴더(`ha_copy_log_base`) | `repl_log_home`, 없으면 그 노드의 `$CUBRID_DATABASES`. 기본값과 다를 때만 그 노드의 cubrid_ha.conf에 씀. S11에서 미리 만듦(restoreslave가 realpath로 계산하므로 있어야 함) |
| 받는 백업 폴더 | `remote_backup_path`, 없으면 그 노드의 `$CUBRID_DATABASES/ha_add_nodes_backup` |
| 기존 노드의 새 슬레이브 로그 폴더 | 그 노드의 `ha_copy_log_base`, 없으면 그 노드의 `$CUBRID_DATABASES` |

- `$CUBRID_DATABASES`는 그 노드 계정의 로그인 셸 환경에서 읽음(csh·tcsh 계정도, 9.1).
- `plan`의 `node_paths[]`에 새 노드·DB마다 볼륨·로그·lob·복제 로그·받는 폴더 경로가 나옴.
- 실측(11.4.5.1908): 새 노드의 `$CUBRID_DATABASES`를 M과 다른 `/home/cubrid/altdb`로 둔 경우, 볼륨·로그가 그 아래에 복원되고 복제 적용 정보의 로그 경로도 그 노드 기준이며 복제가 됨.

### 2.6 새 노드의 다른 싱글 DB

- 새 노드의 databases.txt에 있고 D가 아닌 DB마다 S11에서 cubrid.conf에 `[@db] ha_mode=off`를 넣음(2.4와 같은 방식). 그래야 `[common]`이 HA가 된 뒤에도 그 DB를 싱글로 띄울 수 있음.
- 그 DB가 떠 있으면 S15에서 HA 시작 전에 멈추고(cub_master가 HA로 다시 뜨면서 어차피 내려감) HA 시작 뒤 싱글로 다시 띄움. 확인 키 `confirm_new_node_db_restart`가 있어야 하며 없으면 거절함.
- `plan`의 `new_node_databases[]`에 노드마다 떠 있는 DB(`running`)와 싱글로 남을 DB(`staying_single`)가 나옴.
- 실측: 새 노드에서 cubdb가 떠 있는 상태로 추가 → cubdb는 `[@cubdb] ha_mode=off`로 싱글로 다시 뜨고, D는 HA로 뜸.

## 3. 입력 — 위자드 → CMS → 스크립트

값은 모두 인자로 넘김. 스크립트 파일은 고치지 않음.

### 3.1 위자드 → CMS (요청 JSON)

```
{
  "task": "ha_add_nodes", "action": "run",
  "master_host": "nodeA",
  "db_names": ["db1", "db2"],                  // 싱글일 때만: HA로 넣을 DB
  "confirm_master_restart": "yes", "confirm_other_db_restart": "yes",
  "confirm_new_node_db_restart": "yes",          // 새 노드에 다른 DB가 떠 있을 때
  "new_nodes": [
    {"role": "slave",   "host": "nodeX1", "ssh_port": 22, "os_user": "cubrid", "hostkey_sha256": "SHA256:...", "os_password": "..."},
    {"role": "replica", "host": "nodeY1", "ssh_port": 22, "os_user": "cubrid", "hostkey_sha256": "SHA256:...", "os_password": "..."}
  ],
  "existing_nodes": [
    {"host": "nodeS1", "ssh_port": 22, "os_user": "cubrid", "hostkey_sha256": "SHA256:...", "os_password": "..."},
    {"host": "nodeR1", "ssh_port": 22, "os_user": "cubrid", "hostkey_sha256": "SHA256:...", "os_password": "..."}
  ],
  "repl_log_home": "", "backup_path": "", "remote_backup_path": "",
  "ha_group": "cubrid", "ha_port_id": 59901,     // 승격할 때만
  "db_accounts": [{"db": "db1", "user": "dba", "password": "..."}],  // plan에만, 선택(기본 키 확인)
  "rollback_convert": "no", "start_ha": "yes"
}
```

선택 값과 기본값(구현 기준)

| 키 | 스크립트 인자 | 기본값 | 뜻 |
|---|---|---|---|
| `master_host` | `--master-host` | L의 `uname -n` | L의 HA 노드 이름. 다른 값이면 거절함 |
| `db_names` | `--db` | 없음(싱글일 때 필수) | 승격할 때 HA로 넣을 DB. HA면 주지 않음(대상은 `ha_db_list`) |
| `repl_log_home` | `--repl-log-home` | 새 노드마다 그 노드의 `$CUBRID_DATABASES` | 새 노드의 복제 로그 상위 폴더. 기본값과 다르면 새 노드의 `ha_copy_log_base`로 씀 |
| `backup_path` | `--backup-path` | L의 `$CUBRID_DATABASES/ha_add_nodes_backup` | L에서 백업을 둘 폴더. 그 아래 `<작업 번호>/<DB>`를 만들고 끝나면 지움 |
| `remote_backup_path` | `--remote-backup-path` | 새 노드마다 그 노드의 `$CUBRID_DATABASES/ha_add_nodes_backup` | 새 노드에서 받은 백업을 둘 폴더. 위와 같음 |
| `ha_group` | `--ha-group` | `cubrid` | 승격할 때만: 새 `ha_node_list`의 그룹 이름. HA면 쓰지 않음(M의 값) |
| `ha_port_id` | `--ha-port-id` | `59901` | 승격할 때만: `ha_port_id`. HA면 쓰지 않음(M의 값을 새 노드에 옮김) |
| `confirm_new_node_db_restart` | `--confirm-new-node-db-restart` | 없음 | 새 노드에 다른 DB가 떠 있을 때 필요(2.6). 없으면 거절 |
| `db_accounts` | 없음(CMS만 씀) | 없음 | `plan`에만 싣는 선택 항목. 화면이 지금 로그인이 살아 있는 DB의 계정만 붙임. CMS가 그 계정으로 기본 키 없는 표를 확인함(10절). 비밀번호는 스크립트·작업 폴더·로그로 가지 않음. `run`에는 싣지 않음 |
| `start_ha` | `--no-start-ha`(no일 때) | `yes` | `no`면 새 노드 HA 시작·확인을 하지 않음 |
| `rollback_convert` | `--rollback-convert`(yes일 때) | `no` | 승격 뒤 단계에서 실패하면 싱글까지 되돌릴지 |

- `ha_group`·`ha_port_id`는 요청 키로 받음(화면에서 숨겨도 기본값이 쓰임). HA에서 시작하는 케이스는 M의 cubrid_ha.conf 값을 그대로 씀.

- 노드 원소의 키는 `host`, `ssh_port`, `os_user`, `os_password`, `hostkey_sha256`이고, `new_nodes` 원소에만 `role`(`slave`/`replica`)이 더 있음. 본문의 "포트·계정·지문"은 이 키들을 줄여 부른 것임.
- `existing_nodes` = L 설정의 `ha_node_list`에서 L(과 새 노드)을 뺀 노드 + `ha_replica_list`에서 L을 뺀 노드. 케이스 3·4는 M이 L이라 빠지고, 케이스 5·6은 M이 들어감(M이 원격이므로).
- 확인 키(`confirm_master_restart`, `confirm_other_db_restart`)는 `run`에만 붙임. `plan`은 확인 키 없이 부르고, 결과의 `confirm_required`에 `run`에 붙여야 할 키가 나옴.

### 3.2 CMS → 헬퍼, CMS → 스크립트

| 받는 쪽 | 무엇을 | 어떻게 |
|---|---|---|
| 헬퍼 | 노드마다 호스트·포트·계정·지문·비밀번호 | fd 3 파이프로 한 줄씩(`호스트<TAB>포트<TAB>계정<TAB>지문<TAB>비밀번호`) 쓴 뒤 닫음. 비밀번호는 넷째 탭 뒤부터 줄 끝까지임(탭이 들어 있어도 됨). 줄바꿈·NUL이 든 비밀번호는 C1에서 거절함 |
| 스크립트 | 비밀번호를 뺀 나머지 전부 | 명령줄 인자. CMS가 셸 없이 execv로 부르므로 값이 셸에서 해석되지 않음 |

스크립트 인자

```
ha_add_nodes.sh <plan|run|cleanup|rollback> --job-dir <작업 폴더> --agent <헬퍼 소켓>
  [--master-host <이름>]
  [--db <DB>]...                                 # 싱글일 때 HA로 넣을 DB, 여러 번
  [--confirm-master-restart] [--confirm-other-db-restart] [--confirm-new-node-db-restart]
  --new <slave|replica>:<호스트>:<포트>:<계정>     # 새 노드, 여러 번
  [--existing <호스트>:<포트>:<계정>]...           # 기존 노드, 여러 번
  [--repl-log-home <경로>] [--backup-path <경로>] [--remote-backup-path <경로>]
  [--ha-group <이름>] [--ha-port-id <포트>]          # 승격할 때만
  [--rollback-convert] [--no-start-ha]
```

- 지문은 스크립트 인자에 넣지 않음. 지문은 헬퍼가 들고 있고 헬퍼가 확인함(스크립트가 다른 지문으로 접속시킬 수 없음).
- 이름은 영문·숫자로 시작하고 영문·숫자·`._-`만 씀(`-`로 시작하면 명령 옵션으로 읽히므로 거절). 경로는 절대 경로이고 공백·따옴표·`..`가 없어야 함. CMS와 스크립트가 둘 다 검사함. 원격 명령에 넣는 값은 모두 따옴표 처리함.
- 인자는 `ps`에 보이지만 비밀번호가 없으므로 문제없음. 같은 인자가 작업 폴더 `args`에도 남아 `rollback`·`cleanup`에서 다시 씀.

## 4. 민감 정보(비밀번호) 전달

### 4.1 방법 비교

| 방법 | 다른 계정 | 같은 계정 | 자식 프로세스 | 디스크 | 판단 |
|---|---|---|---|---|---|
| 명령줄 인자 | `ps`·`/proc/<pid>/cmdline`으로 누구나 봄 | 봄 | 자식 인자로 넘기면 또 보임 | 없음 | 쓰지 않음 |
| 환경 변수 | 못 봄(`/proc/<pid>/environ`은 같은 계정·root만) | 봄 | **모든 자식에게 상속됨**(cubrid 유틸, 원격 명령 등). 지우지 않으면 계속 남음 | 없음 | 쓰지 않음 |
| 0600 임시 파일(tmpfs, 바로 삭제) | 못 봄 | 지우기 전까지 봄 | 없음 | 없음(tmpfs). 단, 죽으면 남을 수 있음 | 차선 |
| 표준 입력 / fd 3 파이프로 스크립트에 넘김 | 못 봄 | 읽기 전까지 `/proc/<pid>/fd`로 가로챌 수 있음 | fd를 닫지 않으면 상속 | 없음 | 좋음. 다만 셸 메모리에 남고, 헬퍼에 다시 넘길 때마다 조심해야 함 |
| **스크립트는 비밀번호를 모르고, 헬퍼 에이전트가 들고 원격 접속을 대신 함** | 못 봄 | 헬퍼는 덤프 금지(`PR_SET_DUMPABLE=0`)라 같은 계정도 메모리·환경을 못 읽음(root 제외) | 상속 없음 | 없음 | **채택** |
| CMS가 원격 접속을 대신 함(스크립트가 CMS에 요청) | 못 봄 | — | — | — | 제외. CMS 재시작에 작업이 묶이고 CMS가 두꺼워짐 |

### 4.2 채택한 경로

| 구간 | 방법 |
|---|---|
| 화면 → CMS | 요청 JSON의 `os_password`(키에 `pass`가 있어 NCA 로그 가림 규칙에 걸림) |
| CMS → 헬퍼 | CMS가 헬퍼를 `cm_ssh agent --socket <작업 폴더>/agent.sock --credentials-fd 3`으로 띄우고, fd 3 파이프에 노드별 정보를 쓴 뒤 닫음. CMS는 요청 JSON의 비밀번호를 지움 |
| 헬퍼 안 | 받은 즉시 덤프 금지·core 끔·메모리 잠금(mlock). 노드마다 ssh 세션을 하나씩 열어 두고 재사용(끊기면 다시 접속) |
| 스크립트 → 헬퍼 | `cm_ssh exec --agent <소켓> <호스트> -- <명령>`, `cm_ssh put --agent <소켓> <호스트> <로컬> <원격>`. 호스트 이름만 넘김 |
| 헬퍼 → 원격 | ssh 비밀번호 인증(password, keyboard-interactive)에만 씀. 원격 명령 문자열에는 넣지 않음 |
| 소켓 | 작업 폴더(700) 안의 유닉스 소켓. 접속한 프로세스의 uid가 같은지 확인(SO_PEERCRED) |
| 끝 | 스크립트가 시작하자마자 소켓으로 `bind <자기 pid>`를 보냄. 헬퍼는 그 pid가 사라지거나 pid의 시작 시각이 달라지면(pid 재사용) 스스로 끝남. 30초 안에 `bind`가 없으면(C4 실패 등) 끝남. 스크립트가 정상으로 끝날 때는 `cm_ssh quit`. C4가 실패하면 CMS가 `quit`을 보냄 |
| 터미널 실행 | 스크립트가 헬퍼를 `--ask`로 띄움. 헬퍼가 터미널에서 노드마다 `read`처럼 에코 없이 물어봄. 스크립트는 여전히 비밀번호를 모름 |

- 남는 위험: 작업이 도는 동안 같은 계정의 다른 프로세스가 소켓을 통해 헬퍼에 원격 명령을 시킬 수 있음. 같은 계정은 이미 CMS 계정 권한을 다 갖고 있으므로 새로 생기는 위험은 아님. 작업이 끝나면 소켓도 사라짐.

## 5. ssh 헬퍼 `cm_ssh` — 라이브러리 일원화

- expect·sshpass·시스템 ssh/scp 같은 외부 도구는 쓰지 않음. 원격 명령과 파일 전송을 모두 이 헬퍼 하나로 함.
- 라이브러리: CUBRID 빌드에 이미 들어 있는 OpenSSL 3.5.7(엔진 3rdparty, 정적 라이브러리) 위에 libssh2 1.11.1 하나만 더해 정적으로 링크함. CMS develop의 in-tree 빌드는 이미 엔진의 OpenSSL을 그대로 씀. 헬퍼는 CUBRID 라이브러리(libcubridcs 등)를 링크하지 않음.
- 코드: 이전 POC의 libssh2 코드(접속, 호스트 키 지문, 비밀번호·keyboard-interactive 인증, 원격 명령, SFTP 이어 올리기, 원격 sha256 확인, 데몬 출력 처리)를 옮겨 씀.
- 하위 명령

| 하위 명령 | 하는 일 |
|---|---|
| `agent --socket S --credentials-fd N` / `--ask` | 접속 정보를 받고 소켓에서 요청을 기다림. 준비되면 표준 출력에 `READY` 한 줄을 쓰고 표준 출력을 /dev/null로 돌림(CMS가 읽고 닫은 파이프에 쓰다 죽지 않게) |
| `bind --agent S <pid>` | 헬퍼가 지켜볼 스크립트 pid를 알려 줌 |
| `hostkey <호스트> <포트>` | 지문만 읽음(인증 없음). `ha_check_ssh`의 첫 호출 |
| `check --agent S <호스트>` | 로그인, `uname -n`, CUBRID 경로·버전 |
| `exec --agent S <호스트> [--daemon] -- <명령>` | 명령을 `/bin/sh -c '<명령>'`으로 감싸 실행함(계정의 로그인 셸이 csh·tcsh여도 같은 문법으로 해석되게). 표준 입력을 원격으로 넘기고, 다 보내면 채널에 EOF를 보냄(받는 쪽 `cat`이 끝나게). 출력은 그대로, 종료 코드도 그대로. `--daemon`은 명령을 `</dev/null >원격 임시 파일 2>&1`로 돌린 뒤 그 파일을 읽음(오래 사는 프로세스가 채널을 쥐지 않게) |
| `put --agent S <호스트> <로컬> <원격>` | SFTP로 올림. 끊기면 올린 자리부터. 끝나면 원격 sha256과 크기 비교 |
| `quit --agent S` | 헬퍼 종료 |

- 원격 노드에는 아무것도 설치하지 않음. 원격에서 할 일(설정 편집, 상태 읽기)은 스크립트의 "노드 동작" 부분으로 함. 실행 방식은 하나로 정함(9.1절): 스크립트 자신을 표준 입력으로 보내 원격 임시 파일(0600, 코드뿐이고 비밀번호 없음)에 다 받은 뒤, 그 파일을 로그인 셸로 표준 입력 없이 실행하고 지움. 표준 입력을 읽는 명령이 스크립트 본문을 먹는 일이 없음. 로컬은 같은 함수를 헬퍼 없이 바로 부름. 그래서 로컬·원격이 같은 코드로 같은 일을 함.
- 인증: 접속 한 번에 인증은 한 번만 함(서버가 password를 받으면 password, 아니면 keyboard-interactive 중 하나). 한 노드에서 인증이 실패하면 그 노드에는 다시 접속하지 않고 작업을 멈춤. faillock이 켜진 서버에서 고객 계정이 잠기는 것을 막기 위함임.
- 빌드: CMS CMakeLists에 libssh2 ExternalProject(sha256 고정)를 두고, OpenSSL은 in-tree면 엔진의 `LIBOPENSSL_*`, standalone이면 `OPENSSL_*`를 씀. 링크 옵션 `-Wl,--exclude-libs`로 정적 라이브러리 기호를 내보내지 않음.
- 설치: `$CUBRID/bin/cm_ssh`, 스크립트는 `$CUBRID/share/cms/ha_add_nodes.sh`(755).
- 라이선스
  - libssh2는 BSD-3-Clause임. 바이너리로 배포할 때 저작권 표시·조건·면책 문구를 문서나 함께 배포하는 자료에 넣어야 함.
  - 그래서 libssh2 소스의 `COPYING`을 `libssh2_license.txt`로 엔진과 같은 위치(`$CUBRID/3rdparty/`, 엔진이 3rdparty 라이선스 텍스트를 설치하는 폴더)에 설치함.
  - OpenSSL은 엔진이 이미 `libopenssl_license.txt`(Apache-2.0)를 설치함.
  - CMS standalone 패키지면 패키지 안의 같은 상대 경로에 둠. 저장소에는 고지 파일과 설치 규칙을 같이 넣음.

## 6. 단계 표 (CMS 단계와 스크립트 단계 구분)

표의 "주체"는 그 일을 하는 구성 요소임. CMS = L의 cub_manager, SC = 스크립트, H = 헬퍼(SC의 요청으로 원격 실행). "앞 조건"은 시작하기 전에 끝나 있어야 하는 것임.

### 6.1 CMS 단계 (L의 CMS, 요청마다)

| no | id | 주체 | 하는 일 | 앞 조건 | 결과 |
|---|---|---|---|---|---|
| C1 | `cms_validate` | CMS | 요청 JSON 형식·이름·경로·역할 검사, 중복 호스트, 새 노드 1대 이상 | — | 틀리면 바로 failure |
| C2 | `cms_jobdir` | CMS | 잠금 잡기(9.2절: 짧은 flock 안에서 잠금 파일을 확인하고 `starting <uuid> <시각>`을 씀. 살아 있는 작업이 있으면 거절), uuid, 작업 폴더(700) | C1 | 작업 폴더 |
| C3 | `cms_agent` | CMS → H | 새 실행 함수로 헬퍼를 setsid로 띄움(fd 0~2와 자격 증명 파이프 fd 3만 남기고 4 이상은 모두 닫음. 파이프의 쓰기 쪽은 CLOEXEC). fd 3에 노드별 정보를 쓰고 닫음. `READY`를 최대 10초 기다림. 소켓 이름은 실행마다 새로 정함(`agent.<실행 번호>.sock`) | C2 | 소켓 준비. 실패하면 헬퍼를 끝내고 잠금·작업 폴더 정리 후 failure |
| C4 | `cms_spawn` | CMS → SC | 스크립트를 새 세션으로 띄움(두 번 fork해 CMS가 거둘 자식이 남지 않게, 인자만, 비밀번호 없음, fd 0~2는 /dev/null, 3 이상 모두 닫음). 스크립트가 시작하자마자 스스로 헬퍼에 `bind <자기 pid>`를 보냄 | C3 | 응답 `uuid`, `job_status`=`running`. 실패하면 헬퍼에 `quit` |
| C5 | `cms_status` | CMS | `ha_add_nodes_status`: 작업 폴더의 `job`·`steps`·`plan`·`log`를 읽고 pid가 살아 있는지 확인 | C4 | 진행 상태 |
| C6 | `cms_cancel` | CMS | `ha_add_nodes_cancel`: 스크립트 pid에만 SIGTERM(한 번만. 프로세스 그룹에는 보내지 않음) | C4 | 스크립트가 9.3절 규칙대로 되돌리기로 들어감 |

### 6.2 스크립트 단계 (`run`)

모든 케이스가 같은 사이클을 돎. 승격 묶음(S4~S8)만 싱글 DB일 때 끼우고, 이미 HA인 케이스에는 그 번호가 없음. 단계 번호는 거꾸로 가지 않음: DB마다·노드마다 하는 단계는 그 단계를 모든 DB·노드에 대해 끝낸 뒤 다음 단계로 감. 단위: 1 = 한 번, 노드 = 노드마다, DB×노드 = DB마다 그리고 새 노드마다. 상태 응답의 `steps[]`에는 `stage`(S 번호), `node`, `db`가 붙음.

| 묶음 | no | id | 단위 | 주체·대상 | 하는 일(명령) | 앞 조건 | 되돌리기 |
|---|---|---|---|---|---|---|---|
| A 사전 확인 | S1 | `precheck` | 1 | SC @L | 인자 검사, L의 상태(싱글/마스터/슬레이브/레플리카), `ha_db_list`, 두 목록과 기존 노드 입력 대조, 백업 자리(`df`), 자동 작업 경고, 기본 키 확인 결과(CMS가 `plan` 전에 `db_accounts`로 확인해 둔 것)를 DB마다 경고로. 아무것도 만들지 않음 | C4 | 없음 |
| | S2 | `node_check [노드]` | 노드(E, X, Y) | SC → H @노드 | 로그인, `uname -n`, CUBRID 경로·버전, 모든 노드 이름이 풀리는지. E: D의 상태(M active, S·R standby), 두 목록이 M과 같은지(순서 포함). X·Y: D가 꺼져 있는지, 그 노드의 다른 DB(등록된 것·떠 있는 것), 노드 기준 경로 계산(2.5) | S1 | 없음 |
| | S3 | `preflight [노드]` | 노드(L, X, Y) | SC(@L), SC → H | 승격 전 사전 점검. L: 백업 폴더 쓰기 시험. X·Y: 받는 폴더·복원 대상 경로·복제 로그 폴더 쓰기 시험, 받는 폴더 여유(백업 크기), 작은 파일 전송·삭제 시험(시험 폴더는 바로 지움) | S2 | 없음(남기는 것 없음) |
| B 승격(싱글만) | S4 | `convert_stop` | 1 | SC @L | 떠 있는 DB 전부 `cubrid server stop` | S3 | 처음에 떠 있던 DB를 싱글로 기동 |
| | S5 | `convert_config` | 1 | SC @L | cubrid.conf `ha_mode=on`·`[@db] ha_mode=off`, cubrid_ha.conf(노드 1개), databases.txt | S4 | `.bak` 복원 |
| | S6 | `convert_restart` | 1 | SC @L | `cub_commdb -A` → `cubrid heartbeat start` | S5 | heartbeat 멈춤, cub_master 내림 |
| | S7 | `convert_verify` | 1 | SC @L | D 모두 active(시작부터 180초까지, 조회 시간 포함) | S6 | 없음 |
| | S8 | `convert_start_others` | 1 | SC @L | 고르지 않았던 DB를 싱글로 다시 기동 | S7 | 그 DB 멈춤 |
| C 백업·전송 | S9 | `backup [D]` | DB | SC @L | `cubrid backupdb -C --no-check <D>@localhost`(DB마다 한 번) | S3(승격이 있으면 S8) | 백업 폴더 지움 |
| | S10 | `send_backup [X/Y, D]` | DB×노드 | SC → H(SFTP) | 백업 파일 + bkvinf 올림, 크기·sha256 확인 | S9 전부 | 받은 폴더 지움 |
| D 새 노드 설정 | S11 | `new_config [X/Y]` | 노드(X, Y) | SC → H @X/Y | cubrid.conf `ha_mode`(on/replica), 그 노드의 다른 DB마다 `[@db] ha_mode=off`(2.6). cubrid_ha.conf(M의 목록 순서 그대로 + 끝에 새 노드, M의 [common] 나머지). 복제 로그 폴더 미리 만듦. databases.txt(D 줄, 노드 기준 경로) | S10 전부 | `.bak` 복원, 없던 파일·폴더는 지움 |
| E 복원 | S12 | `keep_old_db [X/Y, D]` | DB×노드 | SC → H @X/Y | 노드 기준 경로에 있는 기존 볼륨·로그·복제 로그 폴더를 `.ha_add_nodes_<uuid>`로 옮겨 보관 | S11 | 제자리로 |
| | S13 | `restore [X/Y, D]` | DB×노드 | SC → H @X/Y | bkvinf를 노드 기준 로그 경로에 두고 `cubrid restoreslave -s <원본 상태> -m <M> -u -B <받은 폴더> <D>` | S12 전부 | 이 작업이 만들었거나 비어 있던 폴더만 비움 |
| | S14 | `first_copylog [X/Y, D]` | DB×노드 | SC → H @X/Y | `cub_admin copylogdb -L <복제 로그 폴더>/<D>_<M> -m async --start-page-id=-1 <D>@<M>` | S13 전부 | 복제 로그 폴더 지움 |
| F 새 노드 HA 시작 | S15 | `new_ha_start [X/Y]` | 노드(X1..Xa 다음 Y1..Yb) | SC → H | 그 노드의 다른 DB가 떠 있으면 멈춤(동의 키), HA 아닌 cub_master 내림, `cubrid heartbeat start`, 다른 DB를 싱글로 다시 기동 | S14 전부 | `heartbeat stop`, cub_master·다른 DB를 작업 전 상태로(설정 복원 뒤) |
| | S16 | `peer_repl_start [X]` | 노드(a ≥ 2일 때) | SC → H @X | 다른 Xj의 Copylogdb가 없으면 `heartbeat repl start <Xj>` | S15 전부 | `repl stop <Xj>`(띄운 것만) |
| G 기존 노드 등록 | S17 | `existing_config [E]` | 노드(M → S1..SN → R1..RM) | SC(@L) 또는 SC → H | 2.2 표대로 cubrid_ha.conf·databases.txt, `cubrid heartbeat reload`, heartbeat status에 새 노드가 보이는지 확인 | S16 | `.bak` 복원 + reload |
| | S18 | `existing_repl_start [E]` | 노드(a ≥ 1일 때, 같은 순서) | SC(@L) 또는 SC → H | X마다 `cubrid heartbeat repl start <X>` | S17 전부 | X마다 `repl stop <X>`(그 노드의 S17 되돌리기보다 먼저), 새로 생긴 로그 폴더 지움 |
| H 확인·정리 | S19 | `verify [X/Y]` | 노드(X, Y) | SC → H | D 모두 standby, `ha_node_list` 다른 노드 전부의 Copylogdb(최대 120초) | S18 | 없음 |
| | S20 | `verify_existing` | 1 | SC → H | 기존 노드마다 X의 Copylogdb가 있는지 | S19 | 없음 |
| | S21 | `cleanup` | 1 | SC, H | L과 X·Y의 이번 작업 백업 폴더 지움(옮겨 보관한 옛 DB는 남김), `cm_ssh quit` | S20 | 없음 |

- 백업은 DB마다 한 번만 받고, 전송은 새 노드 수만큼 함. 모든 DB의 백업이 S21까지 L에 남음.
- 새 노드 설정(S11)은 백업·전송 뒤에 함. 그래서 백업·전송에서 실패하면 새 노드에 바뀐 설정이 없음(받은 백업 폴더만 되돌리기가 지움). restoreslave는 시작할 때 cubrid.conf(HA 모드)·cubrid_ha.conf(D와 `-m` 노드)·databases.txt(D 줄)를 읽으므로 S13 전에만 있으면 됨(소스).
- 단계 수(그 케이스에 있는 단계만): 1 + (|E| + a + b) + (1 + a + b) + [승격 5] + k + k(a + b) + (a + b) + 3k(a + b) + (a + b) + [a ≥ 2이면 a] + (1 + |E|) + [a ≥ 1이면 1 + |E|] + (a + b) + 1 + 1. |E|는 L을 뺀 기존 노드 수. 정확한 표는 `plan`이 돌려주고, 화면은 그 표를 씀.

### 6.3 `plan`, `cleanup`, `rollback`

| 동작 | 단계 |
|---|---|
| `plan` | C1~C4 → S1, S2, S3 → 결과(`plan` 파일: 노드 역할, 바뀔 목록 미리 보기, 재시작될 DB, 새 노드별 다른 DB·경로, 전송량, 경고, 6.2의 단계 표) → 헬퍼 종료. 아무것도 남기지 않음(S3의 시험 폴더도 지움) |
| `cleanup` | C1~C4(새 노드 비밀번호만) → 옮겨 보관한 옛 DB 폴더 지움 |
| `rollback` | C1~C4(작업에 관계된 노드 비밀번호) → `undo` 기록을 거꾸로 실행. 스크립트가 죽어 `interrupted`가 된 작업에 씀 |

### 6.4 승격이 백업보다 먼저인 이유 (실측)

- DB 서버는 HA가 꺼져 있으면 복제용 로그 레코드를 쓰지 않음(소스).
- 실측(11.4.5.1908): 싱글 상태에서 온라인 백업 → 싱글 동안 행 입력 → 승격 → 행 입력 → 그 백업으로 새 슬레이브 복원(`-s master`)·첫 로그 복사·HA 시작 → 등록 → 행 입력. 새 슬레이브는 오류 없이 standby가 되고 복제 적용 위치도 로그 끝까지 갔으나, 싱글 동안 넣은 행은 없고 승격 뒤에 넣은 행만 있었음. 보관 로그는 남아 있었고 재시작으로 로그가 끊기지도 않았음.
- 그래서 백업(S9)은 승격(S4~S8) 뒤에만 받음. 승격 전에 할 수 있는 확인은 S1~S3에 모음.
- 승격 뒤 단계에서 실패하면 기본은 승격 상태 유지(노드 1개 HA 마스터로 서비스 정상, 되돌리면 한 번 더 중단). `rollback_convert=yes`면 싱글까지 되돌림.

## 7. CMS와 스크립트 사이의 의존성

CMS에 HA 단계를 두지 않는 구조로 의존성을 대부분 없앰. 남는 것과 처리 방법은 다음과 같음.

| no | 무엇이 끝나야 | 무엇을 할 수 있음 | 처리 | 대기 |
|---|---|---|---|---|
| P1 | C2 작업 폴더 | C3·C4 | CMS 안에서 차례로 함 | 없음 |
| P2 | C3 헬퍼 준비(비밀번호 받음, 소켓 열림) | S2(첫 원격 접속) | CMS가 `READY`를 받은 뒤에만 C4를 함. 스크립트도 S2 앞에서 소켓을 최대 10초 확인함 | CMS가 최대 10초 |
| P3 | 스크립트의 상태 기록 | C5 상태 응답 | 스크립트가 `job`·`steps`를 임시 파일에 쓰고 mv로 바꿔 넣음. CMS는 그때그때 읽기만 함. 진행 중인 단계가 없는 틈(단계 사이, plan 쓰는 중)에는 CMS가 다음 `pending` 단계를 `step_no`로 줌 | 없음 |
| P3a | 스크립트의 첫 상태 기록 | C4 응답 | CMS는 스크립트가 첫 `job`을 쓸 때까지(새 pid가 보일 때까지, 최대 10초) 기다린 뒤 응답함. 스크립트는 시작하자마자 단계 표(`precheck` 진행 중)를 먼저 쓰고 `job`(`status=running`, `rollback`이면 `phase=rollback`)을 씀. 그래서 응답 직후의 상태 조회가 진행 중 단계를 갖고, `rollback`·`cleanup`에서 같은 폴더의 옛 끝 값을 돌려주지 않음 | CMS가 최대 10초 |
| P4 | C6 취소 신호 | 되돌리기 시작 | 스크립트가 TERM을 받으면 표시만 하고, 지금 명령이 끝나면 되돌리기로 감. `job`에 `phase=rollback`을 씀 | 없음(화면은 상태로 봄) |
| P5 | 스크립트 종료 | 헬퍼 종료 | 스크립트가 `cm_ssh quit`. 죽었으면 헬퍼가 스크립트 pid를 보고 스스로 끝남 | 없음 |
| P6 | S6(L의 cub_master 재기동) | L의 CMS 계속 동작 | `cub_commdb -A`는 CMS를 내리지 않음(이전 POC 실측). CMS의 DB 감시·조회가 잠깐 실패할 수 있음 | 없음 |
| P7 | CMS 재시작 | 작업 계속 | 스크립트·헬퍼가 setsid라 영향 없음. 새 CMS는 작업 폴더로 상태를 읽음 | 없음 |
| P8 | 작업이 도는 동안 | 같은 노드의 다른 CMS 작업(DB 시작·중지, HA 시작·중지, 백업 등) | 잠금 파일을 CMS의 HA·DB 변경 task들이 확인하고 거절하는 것을 권장함(이번 POC 범위 밖, 화면에서 막음) | — |
| P9 | `rollback`·`cleanup` | 비밀번호 | 비밀번호는 작업 폴더에 없으므로 C3을 다시 함(화면이 다시 입력받음) | — |
| P10 | C2 잠금 | 같은 때 들어온 다른 요청 | C2가 flock 안에서 잠금 파일을 확인·기록함(9.2). 둘 중 하나만 통과함 | 없음 |
| P11 | 작업 중 L에서 띄운 DB 서버·복제 프로세스 | 다음 작업의 C2 | 잠금을 fd로 오래 잡지 않으므로(9.2) 물려받을 것이 없음. 로컬 데몬은 setsid·표준 입출력 리다이렉트로 띄움(9.1) | 없음 |
| P12 | CMS 자동 작업(예약 백업, 자동 볼륨 추가) | S4(DB 멈춤)·S9(백업)와 겹침 | 막지 못함. `plan`이 D에 걸린 자동 작업을 찾아 경고하고, 겹치지 않는 시간에 돌리라고 알림(미확인: 겹칠 때의 실제 영향) | — |
| P13 | 원래 작업의 헬퍼 | `rollback`·`cleanup`의 새 헬퍼 | 소켓 이름을 실행마다 새로 정함. 옛 헬퍼가 살아 있으면 CMS가 먼저 `quit`을 보냄 | 없음 |

- 검토했으나 쓰지 않은 구조: CMS가 스크립트를 단계별로 나눠 차례로 부르는 방식(각 단계가 끝나면 CMS가 다음 단계를 부름). 중간에 기다림이 없어지는 대신 CMS가 흐름을 쥐게 됨. 그러면 CMS가 재시작될 때 작업이 멈추고, 되돌리기 순서도 CMS가 알아야 함. 그래서 스크립트 한 번 실행으로 끝까지 가고 CMS는 읽기만 하는 구조를 고름.

## 8. 케이스별 흐름 (1:N:M)

모든 케이스의 공통 순서: C1 → C2 → C3 → C4 → S1 → S2 → S3 → [S4~S8 승격, 싱글만] → S9 → S10 → S11 → S12 → S13 → S14 → S15 → [S16] → S17 → [S18] → S19 → S20 → S21. C5는 화면이 계속 부르고, C6은 사용자가 취소할 때 부름.

| 케이스 | L | 백업 원본(S9) | S2 대상 | B 승격(S4~S8) | S15 | S16 | S17·S18 대상 | `restoreslave -s` |
|---|---|---|---|---|---|---|---|---|
| 1 | 싱글 서버 | L | X1..Xa(기존 노드 없음) | 있음 | X만 | a ≥ 2 | L(M)만, S18은 X마다 | master |
| 2 | 싱글 서버 | L | Y1..Yb | 있음 | Y만 | 없음 | L(M)만, S18 없음 | master |
| 3 | M | L | S1..SN, R1..RM, X | 없음 | X | a ≥ 2 | M, S1..SN, R1..RM | master |
| 4 | M | L | S1..SN, R1..RM, Y | 없음 | Y | 없음 | M, S1..SN, R1..RM(S18 없음) | master |
| 3+4 | M | L | E 전부, X, Y | 없음 | X 다음 Y | a ≥ 2 | E 전부 | master |
| 5 | Sj | L(Sj) | M(원격), 다른 S, R, X, Y | 없음 | X 다음 Y | a ≥ 2 | M(원격), L(로컬), 나머지 S, R | slave |
| 6 | Rj | L(Rj) | M(원격), S, 다른 R, X, Y | 없음 | X 다음 Y | a ≥ 2 | M(원격), S, L(로컬), 나머지 R | replica |

- N = 0, M = 0: E = {M}. S2·S17·S18은 M 하나만 대상임.
- a = 0: S15는 Y만, S16·S18이 없음. 기존 노드는 `ha_replica_list`만 바뀜.
- b = 0: S15는 X만. 기존 노드의 `ha_replica_list`는 그대로임.
- N, M이 클 때
  - S2·S17·S18은 노드 수만큼 차례로 돎. 실패한 노드에서 멈추고, 앞 노드들은 되돌림.
  - 노드 수가 늘면 노드마다 복제 프로세스가 늘어남(마스터·슬레이브는 (|ha_node_list| − 1) × k개씩 copylogdb·applylogdb, 레플리카는 |ha_node_list| × k개씩). `plan`에 노드별로 늘어나는 프로세스 수를 보여 줌.
  - DB 서버의 HA 예약 연결 수는 서버가 뜰 때 정해짐. 노드를 늘린 뒤 추가된 복제 연결은 일반 연결 자리(`max_clients`)를 씀. `plan`이 M의 `max_clients` 여유를 확인해 모자라면 알림.
  - 전송량은 DB 전체 백업 크기 × (a + b)임. 보관 로그가 첫 백업부터 마지막 새 노드의 HA 시작까지 남아 있어야 하므로, a + b가 크면 보관 로그 보존 설정을 미리 늘려야 함(`plan`이 전송량을 알려 줌).
  - 엔진 헤더에 heartbeat 노드 수 상수(8)가 있으나 11.4 소스에서 쓰는 곳을 찾지 못함. 큰 구성(노드 9대 이상)은 실측 항목으로 둠.
- 케이스 5·6은 실측 전까지 막아 둠(`plan`이 `unsupported_case`로 거절). `-s slave`·`-s replica`면 엔진이 원본의 복제 적용 위치(db_ha_apply_info)를 그대로 두는데, 첫 로그 복사가 그 위치부터 지금까지의 틈을 메우는지 확인되지 않았기 때문임(슬레이브·레플리카는 반영이 늦어 틈이 더 큼).
- 케이스 5(미검증, 선택)
  - Sj의 standby DB를 온라인 백업해 `restoreslave -s slave -m <M>`로 복원하고, 첫 로그 복사는 M에서 받음.
  - 매뉴얼의 슬레이브 원본 절차는 restoredb와 보관 로그 복사를 씀. 이 방식으로 대신할 수 있는지는 실측해야 함.
  - 마스터 부하가 줄어드는 효과(백업 I/O)는 실측 뒤에 적음.
- 케이스 6(레플리카에서 실행, 선택. 가능 여부·제약만)
  - 가능해 보이는 근거: restoreslave의 `-s`는 `replica`를 받음(매뉴얼). 흐름은 케이스 5와 같고 백업 원본만 레플리카임.
  - 제약 1: 레플리카는 항상 async로 받으므로 슬레이브보다 반영이 늦을 수 있음. 새 노드는 복원 뒤 M에서 첫 로그를 받으므로 그 차이는 M의 보관 로그가 메움. 그만큼 보관 로그가 더 남아 있어야 함.
  - 제약 2: 레플리카는 엔진이 `force_remove_log_archives=yes`로 고정함. 백업 원본으로만 쓰므로 문제는 없어 보이나 실측이 필요함.
  - 제약 3: 레플리카 노드의 CMS가 M·슬레이브를 원격으로 다룸(케이스 5와 같음).
  - 확인할 것: 레플리카 백업 + `restoreslave -s replica -m <M>`로 만든 새 슬레이브·레플리카가 따라붙는지(12절).

## 9. 되돌리기·취소·재시작

- 되돌리기 기록: 바꾸는 명령을 하기 직전에 되돌리는 방법을 작업 폴더의 `undo`에 한 줄씩 적음(노드, 종류, 경로. 비밀번호 없음). 실패·취소 때 이 파일을 거꾸로 실행함. 스크립트가 중간에 죽어도 같은 기록으로 나중에 되돌릴 수 있음(`rollback`).
- 1:N:M에서의 순서
  - 기록이 실행 순서대로 쌓이므로 거꾸로 실행하면 R1..RM, S1..SN, M 순서로 되돌아가고, 같은 노드 안에서는 `repl stop <X>`가 설정 복원보다 먼저 옴. 노드가 목록에서 빠진 뒤의 `repl stop`은 실패하면서도 마지막 줄을 success로 찍으므로(이전 POC 실측), 결과는 출력의 실패 문구로 판단함.
  - 새 노드는 Y1..Yb, X1..Xa 순서로 heartbeat를 멈춘 뒤 D마다 S13~S10을 거꾸로 되돌림.
  - 일부 기존 노드만 X를 알고 있는 동안에도 서비스에는 영향이 없음. heartbeat는 모르는 노드가 보낸 상태는 버리지만 heartbeat 요청에는 응답함. 그래서 X는 M이 마스터인 것을 알게 되고 혼자 마스터가 되지 않음.
- 승격(케이스 1·2) 뒤 단계에서 실패하면 기본으로 승격 직후 상태(노드 1개 HA 마스터)까지만 되돌림. 승격까지 되돌리면 DB가 한 번 더 재시작되기 때문임. `--rollback-convert`면 싱글까지 되돌림.
- 취소는 9.3절.
- CMS 재시작: 영향 없음(7절 P7). 스크립트가 죽어 있는데 상태가 running이면 `interrupted`로 보이고, `rollback`으로 되돌릴 수 있음.
- 한 번에 한 작업은 9.2절.

### 9.1 명령 실행 규칙

- 오류 처리: `set -e`에 기대지 않음. 명령마다 종료 코드와 출력의 실패 문구를 직접 봄(`repl stop`처럼 실패해도 success를 찍는 명령이 있음). `set -x`는 쓰지 않음.
- 표준 입력: 노드 동작 안의 외부 명령은 모두 `</dev/null`로 돌림(DBA 비밀번호 프롬프트 같은 것이 멈춰 기다리거나 입력을 먹지 않게).
- 원격 노드 동작: `cm_ssh exec --agent S <노드> -- 'f=$(mktemp) && cat >"$f" && <셸 실행> "$f" node <동작> <인자> </dev/null; rc=$?; rm -f "$f"; exit $rc'`에 스크립트 자신을 표준 입력으로 보냄(헬퍼가 `/bin/sh -c`로 감싸고, 다 보낸 뒤 EOF).
  - `<셸 실행>`: S2가 계정의 로그인 셸(`getent passwd`)을 확인함. bash·sh면 `bash -l`. csh·tcsh면 CUBRID 환경이 `.cshrc`에 있으므로, 그 셸로 `CUBRID`·`CUBRID_DATABASES`·`PATH`·`LD_LIBRARY_PATH`를 읽어 와 `env … bash`로 실행함. 로그인 셸 초기화 파일이 찍는 글이 결과에 섞이므로, 노드 동작은 결과를 `@@BEGIN <실행 번호>`와 `@@END <종료 코드>` 사이에만 찍고 스크립트는 그 사이만 읽음.
- 오래 사는 프로세스를 남기는 명령(`cubrid server start`, `cubrid heartbeat start`, `cubrid heartbeat repl start`, cub_master)
  - 원격: `cm_ssh exec --daemon`.
  - 로컬(L): `setsid <명령> </dev/null >작업 폴더의 임시 파일 2>&1`로 띄우고 끝나기를 기다린 뒤 그 파일을 읽음. 스크립트는 오래 열어 두는 fd(잠금 등)가 없으므로(9.2) 물려줄 것이 없음.

### 9.2 한 번에 한 작업(잠금)

- 잠금 파일 `$CUBRID/tmp/ha_add_nodes/lock`의 내용으로 판단하고, 파일을 고칠 때만 짧게 flock을 잡았다가 바로 놓음. 작업 내내 잡고 있는 fd가 없으므로, 작업 중 띄운 DB 서버·복제 프로세스가 잠금을 물려받는 일이 없음.
- 내용: `starting <uuid> <시각>`(C2가 씀) → `running <uuid> <스크립트 pid> <pid 시작 시각>`(스크립트가 시작할 때 씀) → 끝나면 비움.
- 판단: `running`이면 그 pid가 살아 있고 시작 시각이 같을 때만 사용 중임. `starting`이면 60초가 지나면 버려진 것으로 봄. 같은 때 들어온 두 요청은 flock 안에서 차례로 보므로 하나만 통과함.
- `rollback`·`cleanup`은 원래 작업의 스크립트·헬퍼가 살아 있으면 거절함(옛 헬퍼가 남아 있으면 CMS가 먼저 `quit`을 보냄).
- `rollback_blocked`로 끝난 작업은 잠금에 `blocked <uuid>`를 남기고, `run`·`rollback` 작업의 스크립트가 끝나지 않고 사라지면(`interrupted`) `running` 내용이 그대로 남아 잡힌 것으로 봄. 둘 다 그 작업의 `rollback`만 통과하고 다른 작업은 "먼저 되돌리라"로 거절함. 그 작업의 `rollback`을 띄우지 못하면 CMS가 잠금을 원래 내용으로 돌려놓음.
- 승격 경계: 승격 단계가 끝나면 `undo`에 `C <단계 번호>` 줄을 씀. `rollback`(동작 포함)은 원래 작업의 `--rollback-convert`를 따라, 없으면 그 번호까지의 줄을 건너뜀(승격 유지).

### 9.3 취소

- CMS는 스크립트 pid에만 SIGTERM을 보냄. 스크립트는 trap에서 표시만 함.
- 단계마다 취소가 언제 먹는지를 정해 둠.

| 단계 | 취소 시점 | 이유 |
|---|---|---|
| S9 `backup`(로컬) | backupdb가 끝난 뒤(기본) | `backupdb -C`는 백업을 DB 서버가 씀. 클라이언트를 끝내도 서버 쪽 백업이 바로 멈추는지 확인되지 않음. 실측에서 바로 멈추는 것이 확인되면 "바로"로 바꿈 |
| S10 `send_backup` | 바로: 헬퍼에 전송 중단을 요청 | 되돌리기가 받은 폴더를 지움 |
| S13 `restore`, S14 `first_copylog`(원격) | 원격 명령이 끝난 뒤 | 도는 중에 되돌리기(볼륨 지움)와 겹치지 않게. 기다리는 동안 상태에 "취소 대기: <노드> <명령> 실행 중, 경과 시간" 표시 |
| S4~S8 승격, S15·S16 HA 시작, S17·S18 등록 | 그 단계의 명령이 끝난 뒤 | 반쯤 바뀐 HA 상태를 남기지 않게 |
| 그 밖 | 다음 단계로 넘어가기 전 | |

- "끝난 뒤" 단계의 최대 대기: 그 명령의 단계 제한 시간(S9·S13 기본 6시간, S14 1시간, 그 밖 10분, 인자로 바꿀 수 있음)을 넘으면 기다림을 멈추고 상태에 "원격 명령 응답 없음(<노드> <명령>)"을 남긴 채 되돌리기를 하지 않고 멈춤(`job_status`=`rollback_blocked`). 사용자가 그 노드를 확인한 뒤 `rollback`을 다시 부름. 아직 돌고 있을 수 있는 명령 위에 되돌리기를 겹치지 않기 위함임.
- 시간 초과의 판단은 명령 종류로 나눔(실패 목록표 2.1·2.6). 오래 걸리거나 CUBRID 프로세스를 띄우고 내리는 명령(restoreslave, 첫 copylogdb, backupdb, heartbeat·server·cub_master, `reload`, `repl`)만 위와 같이 `rollback_blocked`로 멈춤. 파일·폴더 고치기와 조회는 노드에서 바로 끝나므로, 답이 없으면 연결이 끊긴 것으로 보고(`error`=`no_answer`. 접속 자체가 안 되는 `unreachable`과 구분) 되돌림. 망이 패킷을 버리는 끊김은 헬퍼가 알아차리지 못하고 제한 시간으로만 드러나기 때문임.
- 시간 초과로 명령을 끝낼 때는 KILL을 보냄(되돌리기 중 무시하는 TERM은 띄운 명령에도 무시된 채 물려감).
- 되돌리기 중에는 TERM을 무시함(되돌리기 명령은 취소되지 않음).
- 기다리는 명령은 백그라운드로 띄우고 `wait`함. 그래야 trap이 바로 돌아 "취소 대기" 상태를 남길 수 있음.

### 9.4 되돌리기 기록(`undo`)

- 한 줄에 하나: `<번호> <단계 번호> <노드> <종류> <인자…>`. 종류는 `restore_file <경로> <백업 경로>`, `remove_created <목록 파일>`, `move_back <원래 경로> <옮긴 경로>`, `heartbeat_stop`, `repl_stop <노드>`, `reload`, `server_start <DB>` 등.
- 백업 파일 이름은 `<파일>.bak.<uuid>.<단계 순번>`임. 한 작업에서 같은 파일을 두 번 고쳐도(케이스 1·2에서 L의 cubrid_ha.conf·databases.txt는 S5와 S17에서 바뀜) 백업이 겹치지 않음. 그래서 거꾸로 되돌리면 싱글 시절 파일까지 돌아감.
- 지우는 되돌리기는 경로째 지우지 않음. 그 단계가 만든 파일·폴더만 지움. 예: S12가 반만 옮긴 상태에서 S13 되돌리기가 남은 옛 파일까지 지우는 일이 없음(S13은 S12가 만들었거나 비어 있던 폴더만 비움).
- 되돌린 줄은 `done <번호>`를 덧붙여 적음. `rollback`을 다시 돌리면 끝난 줄은 건너뜀.
- 접속이 안 되는 노드의 줄은 건너뛰고 계속 감. 한 노드의 되돌리기가 제한 시간이나 헬퍼 오류로 실패하면 그 노드의 남은 줄은 이번에는 시도하지 않음. 끝에 "되돌리지 못한 항목"(노드, 종류, 경로)으로 상태에 남김.
- `server_start <DB>`(새 노드·L의 다른 DB를 다시 띄우기)는 떠 있지 않은 DB만 띄움. 기록은 설정 복원보다 뒤에 돌도록 S11(S4)에 남기는데, S15 전에 실패하면 그 DB는 아직 떠 있기 때문임.
- 새 노드에서 작업이 만든 폴더
  - S12가 만든 DB 볼륨·로그 폴더(없던 상위 폴더 포함): 만든 즉시 `rmdir_empty`를 기록함. 그 DB의 S13 전에 실패·취소해도 빈 폴더가 남지 않음. S13이 시작되면 S13 기록이 먼저 그 안을 비움
  - S15 heartbeat start가 만드는 복제 로그 폴더(`<복제 로그 위치>/<DB>_<다른 노드>`): heartbeat start 전에 없던 것을 기록해 heartbeat stop 뒤 지움. S12가 같은 이름의 옛 폴더를 옮겨 두므로 없던 것은 이 작업의 것임
- `rollback` 동작(중단·막힌 작업 되돌리기)은 원래 작업의 단계 표를 읽어 단계마다 `rolled_back`·`rollback_failed`를 표시함. 끊길 때 돌던 단계는 `failed`(interrupted)로 바꿈.

## 10. CMS API

모두 admin 전용. 요청·응답은 CMS JSON 그대로.

| task | 동기/비동기 | 요청 | 응답 |
|---|---|---|---|
| `ha_check_ssh` | 동기(최대 약 20초) | 이전과 같음(`host`, `ssh_port`, `os_user`, `os_password`, `hostkey_sha256`) | 이전과 같음. 내부는 `cm_ssh hostkey`, `cm_ssh check`(일회용 헬퍼) |
| `ha_add_nodes` | 비동기 | 3.1의 JSON. `cleanup`·`rollback`은 `uuid` + 필요한 노드의 접속 정보·비밀번호 | `uuid`, `job_status`=`running` |
| `ha_add_nodes_status` | 동기 | `uuid`, `offset` | `job_status`, `phase`, `step_no`, `step_count`, `steps[]`(`id`, `no`, `title`, `node`, `db`, `state`, `detail`), `nodes[]`(`host`, `role`, `new`), `plan`(plan일 때), `output`, `next_offset`, `error`, `message`, `downtime_sec`, `kept_db_paths`, `unrestored[]`(`node`, `kind`, `path`: 되돌리지 못한 항목, 9.4), `preflight_failures[]`(`node`, `path`, `reason`=`not_writable`/`no_room`/`cannot_create`/`transfer_failed`, `detail`: S3 사전 점검이 찾은 것) |
| `ha_add_nodes_cancel` | 동기 | `uuid` | `job_status` |

- `plan` 결과(상태 응답의 `plan`)

```
{
  "local": {"host": "nodeA", "state": "single|master|slave|replica"},
  "case": 1,                                   // 0절 케이스 번호(새 노드 역할이 섞이면 1·2 또는 3·4를 같이 뜻함)
  "master_host": "nodeA",
  "db_names": ["db1", "db2"],                  // 대상 DB: HA면 ha_db_list, 싱글이면 요청의 db_names
  "nodes": [{"host": "nodeA", "role": "master|slave|replica", "new": false, "version": "11.4.5.1908", "local": true}, ...],
  "convert": {                                 // 싱글일 때만, 아니면 null
    "ha_databases": [...], "restart_databases": [...],
    "other_running_databases": [...], "single_databases": [...]
  },
  "confirm_required": ["confirm_master_restart", "confirm_other_db_restart", "confirm_new_node_db_restart"],
  "new_node_databases": [{"host": "nodeX1", "running": ["db9"], "staying_single": ["db9", "db8"]}, ...],
  "node_paths": [{"host": "nodeX1", "db": "db1", "vol": "...", "log": "...", "lob": "file:...", "repl_log_home": "...", "receive": "..."}, ...],
  "changes": [{"host": "nodeS1", "file": "cubrid_ha.conf", "key": "ha_node_list", "before": "...", "after": "..."}, ...],
  "backup_bytes": 0, "transfer_bytes": 0,
  "process_increase": [{"host": "nodeS1", "copylogdb": 2, "applylogdb": 2}, ...],
  "primary_key_check": [{"db": "db1", "status": "ok|partial|failed|not_logged_in", "tables": ["DBA.t9"], "message": ""}, ...],
  "warnings": ["..."],
  "steps": [{"no": 1, "stage": "S1", "id": "precheck", "title": "...", "node": "", "db": "", "actor": "script"}, ...]
}
```

- 기본 키 없는 표 확인: HA는 기본 키가 없는 표를 복제하지 않음.
  - `plan` 요청의 `db_accounts[]`(db, user, password)로 CMS가 DB마다 카탈로그를 조회함(`db_class`에서 기본 키 인덱스가 없는 사용자 표). 결과만 작업 폴더에 남기고 스크립트가 `plan`에 넣음.
  - 비밀번호 경로: csql은 표준 입력이 터미널일 때만 비밀번호를 물으므로(소스), CMS가 가상 터미널(pty)로 csql을 띄우고 프롬프트에 비밀번호를 씀. 명령줄 인자·환경 변수·파일에 넣지 않음. 다시 물으면(틀린 비밀번호) 바로 끝냄.
  - `plan.primary_key_check[]`: `db`, `status`(`ok` / `partial`: dba가 아니라 다른 사용자 표는 못 봄 / `failed`: 로그인·조회 실패 / `not_logged_in`: 계정이 오지 않음), `tables`(소유자.표), `message`. 같은 내용을 `warnings[]`에도 DB 이름과 함께 넣음.
  - 실측: dba로 기본 키 없는 표 2개를 찾음, 비밀번호가 있는 일반 계정은 `partial`과 자기 표, 틀린 비밀번호는 `failed`(login failed), 계정 없는 DB는 `not_logged_in`. 작업 폴더·CMS 로그에 비밀번호 없음.
- `ha_check_ssh`의 첫 호출(지문 받기)은 화면이 노드마다 부름. 사용자가 지문을 확인한 뒤 그 지문을 `hostkey_sha256`으로 넘김.
- CMS가 스크립트를 부르기 전에 확인하는 것: 스크립트·헬퍼가 정해진 경로의 일반 파일이고 그룹·다른 사용자가 쓸 수 없는지.
- 이전 POC의 `ha_convert_master`·`ha_make_slave*` task는 이 API로 바뀜.

## 11. 사전 조건과 거절

- 모든 노드의 CUBRID 전체 버전(예: 11.4.5.1908)이 같아야 함(결정). 다르면 S2에서 `version_mismatch`로 거절하고, 문구에 노드 이름과 양쪽 버전을 넣음. 패치 번호만 다른 노드를 허용하는 선택지는 없음.
- 모든 노드에서 모든 노드 이름이 풀려야 함. HA 노드 이름은 각 서버의 `uname -n`과 같아야 함.
- 기존 노드 입력은 M 설정의 두 목록과 같아야 함(2.2).
- 새 노드: D가 떠 있으면 안 됨. 다른 DB가 떠 있으면 `confirm_new_node_db_restart`가 있어야 함(2.6). 노드 기준 경로(2.5)에 쓸 수 있어야 하고, 받는 폴더에 백업 크기만큼 여유가 있어야 함(S3).
- 백업 자리: 백업 원본에 D 전체 백업 크기, 새 노드마다 받는 폴더에 같은 크기가 있어야 함(`plan`이 `df`로 확인).
- 복제 로그 자리: 새 슬레이브마다 모든 기존 노드(레플리카 포함)와 다른 새 노드에 그 슬레이브의 복제 로그 폴더가 생김. `plan`이 각 노드의 복제 로그 상위 폴더 여유를 D마다 로그 볼륨 크기 × (`ha_copy_log_max_archives` + 1)로 어림해 확인함.
- 방화벽(`ha_port_id` UDP, `cubrid_port_id` TCP)은 확인하지 못함. 안 열려 있으면 S19에서 실패함.

## 12. 확인할 것(N노드 시험 환경에서 실측)

| 항목 | 왜 |
|---|---|
| a ≥ 2일 때 새 노드끼리의 copylogdb가 heartbeat start만으로 뜨고 붙는지 | S16 필요 여부. 결과(새 슬레이브 2대를 한 번에 추가한 시험): heartbeat start만으로 새 노드끼리 서로의 로그를 복사했고 S16은 할 일이 없었음. S16은 빠진 것이 있을 때만 띄우는 단계로 그대로 둠 |
| 새 레플리카: 기존 노드 reload만으로 붙는지, 모든 슬레이브의 로그를 받는지, failover 뒤에도 데이터가 이어지는지 | 2.1 표 |
| `repl start <레플리카>`가 실제로 "not listed"로 거부되는지 | 소스 확인만 함 |
| N, M이 클 때(노드 9대 이상 포함) heartbeat·복제가 정상인지 | 노드 수 상한 |
| 케이스 5: 슬레이브 백업 + `restoreslave -s slave` + M에서 첫 로그 복사로 따라붙는지 | 매뉴얼 절차와 다름 |
| 케이스 6: 레플리카 백업 + `restoreslave -s replica` + M에서 첫 로그 복사로 따라붙는지 | 8절 케이스 6 제약 |
| DBA 비밀번호가 걸린 DB에서 backupdb·restoreslave·changemode가 비밀번호 없이 되는지 | 고객 DB는 대부분 DBA 비밀번호가 있음 |
| 노드를 늘린 뒤 `max_clients`가 거의 찬 M에서 새 노드의 로그 복사가 붙는지 | HA 예약 연결 수 |
| 취소·실패 되돌리기 뒤 모든 노드가 시작 전과 같은지(설정 파일, DB 폴더, HA 프로세스, cub_master) — 실패 지점을 S2·S10·S15·S17(k번째 노드)·S18로 바꿔 가며 | 9절 |
| CMS 재시작 중에도 작업이 끝까지 가는지, 상태 조회가 맞는지 | 7절 P7 |
| 비밀번호가 `ps`, `/proc/*/cmdline`·`environ`, 작업 폴더, 로그, 원격 노드 어디에도 없는지, 헬퍼 메모리를 같은 계정이 못 읽는지 | 4절 |
| L에서 S6·S8·S18 뒤 띄워진 cub_server·copylogdb·applylogdb가 작업 폴더나 잠금 파일의 fd를 쥐고 있지 않은지(`/proc/<pid>/fd`), 작업이 끝난 뒤 다음 작업이 잠금을 얻는지 | 9.1·9.2 |
| S13 도중 취소: X의 restoreslave가 끝난 뒤에 되돌리기가 시작되는지 | 9.3 |
| DBA 비밀번호가 걸린 DB에서 원격 노드 동작이 끝까지 실행되는지(입력을 기다리며 멈추지 않는지) | 9.1 |
| 기존 노드의 복제 로그 폴더 자리 어림이 맞는지 | 11절 |
| `cubrid heartbeat reload`가 실패할 때 종료 코드·출력이 실패를 알려 주는지 | S17 확인 방법 |
| 틀린 비밀번호로 한 번 실패했을 때 접속 시도가 한 번에서 끝나는지(faillock 계정 잠김 방지) | 5절 |
| S9 도중 backupdb 클라이언트를 끝냈을 때 서버 쪽 백업이 바로 멈추는지 | 9.3 S9 취소 시점 |
| 로그인 셸이 csh·tcsh인 계정에서 원격 노드 동작이 되는지 | 9.1 |

## 13. 한계

- 여러 서버의 변경을 한 번에 성공시키거나 한 번에 취소하는 것은 안 됨. 되돌리기는 기록을 거꾸로 실행하는 최선의 노력임.
- 승격과 cub_master 재기동은 그 노드의 떠 있는 DB를 모두 재시작함(서비스 중단).
- 전송은 새 노드마다 차례로 함(병렬 전송·새 노드 사이 중계는 하지 않음).
- 같은 설정 파일을 다른 도구가 동시에 고치는 경우의 충돌 방지는 없음.
- 윈도우는 대상이 아님.
- 대상 OS는 제조사 지원이 끝나지 않은 리눅스뿐임(CentOS 7 같은 지원 종료 OS는 고려하지 않음). 정확한 목록은 실측 문서 맨 위를 따름. 가장 낮은 OpenSSH는 RHEL 8 계열 8.0임.
- 헬퍼에 넣는 OpenSSL은 FIPS 인증 모듈이 아님(부록 3의 OpenSSH 방식이면 OS 모듈을 쓰므로 없어짐).

## 부록. 근거

| 내용 | 근거 |
|---|---|
| copylogdb·applylogdb는 `ha_node_list` 노드만 대상, 레플리카 노드의 복사 모드는 항상 async, 목록에 없는 노드는 "not listed" | 엔진 `src/executables/util_common.c`(HA 설정 읽기), `util_service.c`(copylogdb·applylogdb 시작) |
| 레플리카는 자기 이름이 `ha_replica_list`에 있어야 함, 두 목록의 그룹 이름이 같아야 함 | 엔진 `src/executables/master_heartbeat.c`(노드 목록 읽기) |
| heartbeat 노드 수 상수 8 정의(쓰는 곳 못 찾음) | 엔진 `src/executables/master_heartbeat.h` |
| `[@db]` 절 `ha_mode`가 DB별로 적용됨, off면 cubrid_ha.conf를 읽지 않음 | 엔진 `src/base/system_parameter.c`, `util_service.c`(server start/stop) + 실측 |
| HA 예약 연결 수는 서버가 뜰 때 정해지고, 모자라면 일반 연결을 씀 | 엔진 `src/connection/connection_globals.c`, `connection_sr.c` |
| 엔진 3rdparty OpenSSL 3.5.7, 3rdparty 라이선스 텍스트를 `$CUBRID/3rdparty`에 설치 | 엔진 `3rdparty/CMakeLists.txt`, `3rdparty/license/`, 최상위 `CMakeLists.txt` |
| CMS in-tree 빌드는 엔진의 OpenSSL을 씀 | CMS `server/CMakeLists.txt` |
| 운영 중 슬레이브 추가: 기존 노드 설정 + reload + `repl start <새 노드>` / 레플리카 추가: `ha_replica_list` + reload, 복제 프로세스 띄우지 않음 / restoreslave `-s`·`-m` | CUBRID 11.4 매뉴얼 HA 장 |
| reload만으로는 새 슬레이브용 프로세스가 안 뜸, `repl stop` 순서, 데몬 출력 처리, `cub_commdb -A`는 CMS를 내리지 않음, 정적 OpenSSL 링크 순서 | 이전 POC 실측(브랜치 poc/ha-make-slave) |
| libssh2 라이선스 BSD-3-Clause | libssh2 소스 `COPYING` |

## 부록 2. 단계 번호 대응 (v3 → v4)

| v3 | v4 | id | 바뀐 점 |
|---|---|---|---|
| S1 | S1 | `precheck` | 디스크에 아무것도 만들지 않음 |
| S2 | S2 | `node_check` | 새 노드의 다른 DB·노드 기준 경로 계산 추가 |
| 없음 | S3 | `preflight` | 새로 생김(사전 점검) |
| S3~S7 | S4~S8 | `convert_*` | 번호만 바뀜 |
| S8 | S11 | `new_config` | 백업·전송 뒤로 옮김, 다른 DB `[@db] ha_mode=off`, 복제 로그 폴더 미리 만듦, 노드 기준 경로 |
| S9 | S9 | `backup` | DB 전부를 먼저 백업 |
| S10 | S10 | `send_backup` | 백업 전부 뒤에 전송 |
| S11 | S12 | `keep_old_db` | 노드 기준 경로 |
| S12 | S13 | `restore` | `-u`, bkvinf는 노드 기준 로그 경로 |
| S13 | S14 | `first_copylog` | 번호만 바뀜 |
| S14·S15 | S15 | `new_ha_start` | X 다음 Y를 한 번호로, 새 노드의 다른 DB 멈춤·재기동 |
| S16 | S16 | `peer_repl_start` | 같음 |
| S17 | S17 | `existing_config` | 같음 |
| S18 | S18 | `existing_repl_start` | 같음 |
| S19 | S19 | `verify` | 새 노드 확인만 |
| 없음 | S20 | `verify_existing` | 기존 노드 확인을 따로 뗌(v3에서는 S19 안) |
| S20 | S21 | `cleanup` | 번호만 바뀜 |

## 부록 3. v5 변경안 (검토 중, 코드는 아직 v4)

v4와 달라지는 네 가지를 한데 정리함. (1) 원격 접속을 번들 헬퍼(`cm_ssh`: libssh2 + OpenSSL) 대신 L에 깔린 OpenSSH 클라이언트로 함 (2) 원격 작업을 "스크립트를 임시 파일로 보내 실행"에서 "패키지에 설치된 노드용 스크립트 실행"으로 바꿈 (3) 스크립트 위조 확인(sha256) (4) ssh 원인별 오류. 아래 "결정"은 이미 정한 것이고, 나머지는 제안임.

### 3.0 결정과 범위

| 항목 | 내용 |
|---|---|
| 대상 OS | 제조사 지원이 끝나지 않은 OS만 대상으로 함(정확한 목록은 실측 문서 맨 위). CentOS 7 같은 지원 종료 OS는 고려하지 않음. 그래서 가장 낮은 OpenSSH는 RHEL 8 계열의 8.0임 |
| CUBRID 버전 | 모든 노드의 전체 버전(예: 11.4.5.1908)이 같아야 함. 다르면 S2에서 거절하고 문구에 노드 이름과 양쪽 버전을 넣음(지금 구현: `version_mismatch` "<노드>: CUBRID <그 노드 버전>, this server <L 버전>"). 패치 번호만 다른 노드를 허용하는 선택지는 두지 않고, 버전이 섞인 상황을 위한 처리도 넣지 않음 |
| ssh 오류 | 뭉뚱그린 "연결 실패"를 쓰지 않음. 원인마다 오류 코드·문구·사용자 할 일을 따로 두고, `plan`(C3·S1·S2)에서 거름(3.5) |

CentOS 7 호환 때문에 넣은 우회(표시만 함, OpenSSH 방식이면 헬퍼와 함께 모두 없어짐)

| 우회 | 위치 | 이유였던 것 |
|---|---|---|
| 헬퍼를 공식 빌드 이미지(CentOS 6, glibc 2.12)에서 빌드하고 요구 GLIBC를 2.7 이하로 맞춤 | 빌드 | CentOS 7(glibc 2.17)에서 실행 |
| `accept4` 대신 `accept` + `fcntl(FD_CLOEXEC)` | `cm_ssh.cpp` | `accept4`가 GLIBC_2.10을 요구 |
| `-lrt` 링크 | `CMakeLists.txt` | glibc 2.17 미만의 `clock_gettime` |
| libssh2 `CMAKE_INSTALL_LIBDIR=lib` | `CMakeLists.txt` | CentOS 계열 빌드 이미지가 lib64에 설치 |

- 스크립트 쪽에는 CentOS 7만을 위한 우회가 없음(`timeout`, `flock`, `chmod --reference`, `mktemp`는 대상 OS 모두에 있음).

### 3.1 OpenSSH 방식 — 비밀번호 경로

OpenSSH의 `ssh`는 비밀번호를 명령줄·표준 입력으로 받지 않음. 터미널이 없으면 `SSH_ASKPASS` 프로그램을 실행해 그 출력으로 받음.

- 8.4 이상: `SSH_ASKPASS_REQUIRE=force`면 터미널이 있어도 askpass를 씀
- 8.0~8.3(RHEL 8 계열 8.0): `DISPLAY`가 있고 제어 터미널이 없을 때만 씀. 그래서 `DISPLAY=x`(아무 값)를 주고 `setsid`로 제어 터미널을 떼고 실행함
- 두 가지를 같이 줌(8.4 이상은 앞의 것, 8.0~8.3은 뒤의 것으로 동작)

askpass까지 비밀번호를 넘기는 방법

| 방법 | `ps`·`/proc` 노출 | CMS↔스크립트 의존 | 계정 잠김 방지 | 판단 |
|---|---|---|---|---|
| (a) 환경 변수(askpass가 `$X`를 출력) | `ps`에는 없음. 같은 계정은 `/proc/<pid>/environ`으로 읽음. 그 ssh가 ControlMaster로 작업 내내 살아 있으므로 작업 시간 내내 노출 | 없음 | 노드당 한 번 | 쓰지 않음 |
| (b) 작업 폴더(700) 안 FIFO. CMS가 노드마다 한 번 쓰고 바로 지움 | 없음. 파일에 남지 않음(FIFO는 내용을 저장하지 않음). 같은 계정 프로세스가 먼저 열어 가로챌 틈이 있으나, 같은 계정은 이미 CMS 권한을 다 가짐(4.2의 남는 위험과 같음) | C3에서 CMS가 각 노드의 첫 로그인이 끝날 때까지 기다림(지금의 `READY` 기다림과 같은 자리 하나) | 노드당 로그인 한 번(아래 ControlMaster) | **채택** |
| (c) fd로 넘김 | 없음 | — | — | 쓰지 못함: `ssh`가 시작할 때 3 이상의 fd를 닫음[확인 필요: OpenSSH 소스] |
| (c') 0600 임시 파일을 askpass가 읽고 지움 | 같은 계정만. 디스크에 잠깐 남고, 죽으면 남을 수 있음 | (b)와 같음 | 같음 | (b)보다 못함 |
| (c'') sshpass·expect | sshpass는 `-p`(명령줄)·`-e`(환경 변수) 방식이고 대상 OS 기본 설치가 아님 | — | — | 쓰지 않음 |

채택한 흐름(노드마다, C3에서 CMS가 함)

1. `ssh-keyscan`으로 호스트 키를 받아 화면이 확인한 지문과 비교함. 같은 키 하나만 작업 전용 `known_hosts`에 씀. 그 키 종류만 쓰도록 `HostKeyAlgorithms`를 고정함(OpenSSH 판마다 먼저 고르는 키 종류가 다를 수 있음[실측 항목])
2. 인증 방법 확인: 인증을 하지 않는 접속(`BatchMode=yes`)으로 서버가 허용하는 방법 목록을 읽음. password가 있으면 password만, 없고 keyboard-interactive가 있으면 그것만 쓰도록 `PreferredAuthentications`를 하나로 정하고 `NumberOfPasswordPrompts=1`. 둘 다 없으면 `no_password_auth`로 거절
3. FIFO(이름은 임의, 0600)를 만들고 `setsid ssh -M -N -f -S <ControlPath> …`를 `SSH_ASKPASS=<노드용 askpass>`, `SSH_ASKPASS_REQUIRE=force`, `DISPLAY=x`로 띄움. askpass는 FIFO를 읽어 출력하는 짧은 셸 스크립트(패키지에 설치)
4. CMS가 FIFO에 비밀번호를 한 번 쓰고 닫은 뒤 FIFO를 지움. 메모리의 비밀번호도 지움
5. `ssh -O check -S <ControlPath>`로 로그인이 됐는지 확인. 실패면 원인을 가려(3.5) 그 노드는 다시 시도하지 않음

- "스크립트는 비밀번호를 모름"은 그대로 지켜짐. 스크립트는 ControlPath만 받아 `ssh -S <ControlPath> -o ControlMaster=no <노드> …`로 씀. 비밀번호가 지나가는 곳은 CMS 메모리 → FIFO → askpass 출력 → ssh뿐임
- 달라지는 점: 지금 헬퍼는 비밀번호를 들고 있다가 끊기면 한 번 다시 로그인함. OpenSSH 방식은 ControlMaster가 죽으면 그 노드에 다시 로그인할 수 없음(비밀번호가 남아 있지 않음) → 그 노드는 `connection_lost`. 되돌리기는 사용자가 비밀번호를 다시 넣고 `rollback`(C3를 다시 함). 계정 잠김 쪽으로는 로그인 시도가 더 적어 유리함

### 3.2 `cm_ssh` 하위 명령을 OpenSSH로

| 지금 | OpenSSH 방식 | 비고 |
|---|---|---|
| `hostkey <호스트> <포트>` | `ssh-keyscan -T 10 -p <포트> <호스트>` → `ssh-keygen -lf <파일>`로 SHA256 지문 | 인증 없음. `ha_check_ssh` 첫 호출 |
| `check` | 3.1의 1~5를 일회용 ControlPath로 하고 `uname -n`, CUBRID 경로·버전, 노드 스크립트 버전·sha256을 읽은 뒤 `-O exit` | 로그인 시도 한 번 |
| `agent`(비밀번호 들고 대기) | C3에서 노드마다 ControlMaster(`-M -N -f`, `ControlPersist=yes`: 시간 제한 없음, 정리는 3.8) | 비밀번호는 로그인에만 쓰고 남기지 않음 |
| `bind <pid>`(스크립트가 죽으면 헬퍼도 끝) | 감시 셸(3.8)이 스크립트가 어떻게 끝나든 노드마다 `-O exit`. 감시 셸까지 죽으면 CMS가 `interrupted`를 볼 때·`rollback`·`cleanup`을 받을 때 `-O exit` | 그 사이 같은 계정이 소켓으로 그 노드에 명령할 수 있음(4.2 남는 위험과 같은 성격) |
| `exec` | `ssh -S <cp> -o ControlMaster=no -T <노드> <노드 스크립트 경로> <하위 명령> <인자>` | 원격 셸이 명령 문자열을 해석하므로 인자는 지금처럼 모두 따옴표 처리. csh·tcsh 계정도 경로 + 인자만이라 문법 차이 없음 |
| `exec --stdin` | 같은 명령에 표준 입력을 넘김 | |
| `exec --daemon` | 쓰지 않음(노드 스크립트가 데몬을 `setsid … </dev/null >파일 2>&1`로 띄움. 지금도 같음) | |
| `put`(SFTP, 이어 올리기, 원격 sha256) | 노드 스크립트 `recv <경로> <크기> <sha256>`에 `ssh … <` 로 흘려 보냄. 끊기면 `recv --size`로 받은 크기를 묻고 `tail -c +<크기+1> | ssh … recv --append`로 이어 보냄(최대 3번). 끝에 노드 스크립트가 크기·sha256을 비교 | scp는 OpenSSH 9.0부터 SFTP를 쓰므로 SFTP를 막은 서버에서 안 됨. `cat` 흐름은 SFTP 하위 시스템이 필요 없음 |
| `quit` | `ssh -S <cp> -O exit <노드>` | |
| 취소 중 원격 명령 | 지금과 같음: 로컬 `ssh` 클라이언트만 끝냄. 원격 명령은 채널이 닫혀도 계속 돌 수 있으므로 9.3의 "끝난 뒤" 규칙과 rollback_blocked 판단을 그대로 씀 | |
| 응답 없는 노드 | ControlMaster에 `ServerAliveInterval=15`, `ServerAliveCountMax=20` → 약 5분 무응답이면 마스터가 끝남(몇 분 끊김은 견딤) → 그 뒤 명령은 `-O check` 실패로 바로 `connection_lost`. 지금의 제한 시간 판단(2.1·2.6)은 그대로 둠 | 다시 로그인하지 않음(3.8) |
| ControlPath 길이 | 유닉스 소켓 경로는 108바이트(리눅스)까지. `ssh`가 만들 때 임시 이름(약 17자)을 붙임. 실행마다 새 폴더 `$CUBRID/tmp/hs<임의 8자>/<노드 순번>`(3.8). C3가 길이를 계산해 넘으면 `controlpath_too_long`으로 거절 | `$CUBRID`가 아주 길 때만 걸림 |
| 정리 | 끝(`finish`)에서 노드마다 `-O exit`, 짧은 경로 폴더 삭제 | |

### 3.3 없어지는 것, 새로 생기는 것

| 없어지는 것 | 새로 생기는 것 |
|---|---|
| `cm_ssh`(약 1,700줄), CMakeLists의 libssh2 ExternalProject와 정적 OpenSSL 빌드, `--exclude-libs` 링크 순서 | L에 `ssh`, `ssh-keyscan`, `ssh-keygen`, `setsid`가 있어야 함. C3·`ha_check_ssh`가 확인하고 없으면 무엇이 없는지 밝혀 거절(`local_ssh_missing`) |
| `libssh2_license.txt` 고지 | OpenSSH 판 차이(8.0~9.x): askpass 조건, 먼저 고르는 호스트 키 종류, scp 동작. 실측으로 맞춤 |
| CentOS 6 빌드 이미지·GLIBC 제약(3.0 표) | 노드 스크립트·askpass 설치(3.4) |
| "번들 OpenSSL은 FIPS 인증 모듈이 아님" 한계 | FIPS: OS의 OpenSSH와 OS 암호 모듈을 쓰므로, FIPS 모드 OS에서는 그 OS가 인증받은 모듈로 동작함(13절 한계가 사라짐) |
| 헬퍼의 비밀번호 메모리 보호(mlock, 덤프 금지) | 비밀번호가 CMS 밖에 오래 남지 않으므로 필요 없음(로그인 뒤 버림) |

### 3.4 노드용 스크립트(`node.sh`) 설치형

구조

- CMS 패키지에 노드용 스크립트를 넣음. 경로는 지금 배치 관례(`$CUBRID/share/cms/ha_add_nodes.sh`)에 맞춰 `$CUBRID/share/cms/ha_node.sh`, askpass는 `$CUBRID/share/cms/ha_askpass.sh`
- L의 스크립트는 `ssh <노드> '<그 노드의 $CUBRID>/share/cms/ha_node.sh <하위 명령> <인자>'`만 부름. 인자는 호스트·DB 이름·경로·숫자뿐이고 비밀번호는 없음
- L 자신의 노드 동작도 같은 `ha_node.sh`를 바로 부름(로컬·원격 같은 코드는 그대로 유지)
- 지금 `ha_add_nodes.sh` 안의 "노드 동작" 부분(`node_*` 함수)을 그대로 떼어 냄. `ha_add_nodes.sh`는 흐름만 남음

하위 명령(지금 노드 동작에서 옮김)

| 단계 | 하위 명령 |
|---|---|
| 모든 호출 전(S2) | `version`(스크립트 버전 + CUBRID 전체 버전 + 자기 sha256), `info`(호스트 이름, 로그인 셸, `$CUBRID`, `$CUBRID_DATABASES`, HA 설정, 떠 있는 서버, databases.txt) |
| S2·S3 확인 | `resolve <이름>...`, `free <경로>`, `writable <경로>...`, `mode <DB>...`, `exists <경로>...` |
| S10 받기 | `mkdir <경로>`, `recv <경로> <크기> <sha256> [--append]`, `recv --size <경로>` |
| S11 설정 | `conf_edit <파일> <표시> <절> <키> <값>...`, `dbtxt_put …`, `mkdir` |
| S12 보관 | `keep_old <표시> <DB> <볼륨> <로그> <복제 로그 위치>` |
| S13 복원 | `restore <DB> <백업 폴더> <로그 경로> <마스터> <원본 상태>` |
| S14 첫 복사 | `first_copylog <DB> <마스터> <폴더>` |
| S15·S16 HA 시작 | `server_stop`, `master_down`, `ha_start`, `server_start`, `hb_status`, `repl start <노드>` |
| S17·S18 기존 노드 | `conf_edit`, `dbtxt_add_hosts`, `reload`, `repl start|stop <노드>`, `exists` |
| S19·S20 확인 | `mode`, `hb_status` |
| S21 정리 | `remove <경로>`, `rmdir_empty <경로>`, `drop_backups <표시> <경로>...` |
| 되돌리기 | `undo_file`, `move_back`, `clear`, `remove`, `rmdir_empty`, `server_ensure`, `server_stop`, `ha_stop`, `master_start`, `reload`, `repl stop` |

- 지금의 범용 `cubrid <인자...>` 통로는 없앰(L의 backupdb만 씀 → L 쪽 `backup <DB> <폴더>` 하위 명령으로). 원격에서 아무 명령이나 돌릴 길을 남기지 않기 위함임
- 경로 인자는 지금처럼 `valid_path`로 검사하고, 지우는 하위 명령(`remove`, `clear`)은 작업 표시(`.ha_add_nodes_`·작업 uuid)가 들어간 경로나 작업이 기록한 경로만 받음

버전·위조 확인(S2, 3.6과 같이)

- `ha_node.sh version`이 스크립트 버전과 CUBRID 전체 버전을 알려 줌. L과 다르면 `node_script_version_mismatch`, 파일이 없으면(원격 종료 코드 127 또는 "No such file") `node_script_missing`. CUBRID 버전이 다르면 지금처럼 `version_mismatch`

확인한 것

| 질문 | 답 | 근거 |
|---|---|---|
| CUBRID 배포판에 CMS가 늘 같이 들어가는지 | 공식 빌드는 늘 들어감. 엔진 저장소가 CMS를 하위 모듈(`cubridmanager`)로 두고 `WITH_CMSERVER` 기본값이 ON이며, tarball·shell·rpm이 같은 설치 규칙을 씀. 하위 모듈이 없을 때만 빠짐(소스에서 직접 빌드하는 경우) | 엔진 `CMakeLists.txt`(`option(WITH_CMSERVER … ON)`, `add_subdirectory(cubridmanager/server)`), `build.sh`(`without_cmserver`), `.gitmodules` |
| 그래서 노드 스크립트를 엔진·CMS 중 어디에 둘지 | CMS 쪽. 노드 스크립트는 CMS가 떠 있을 필요가 없고 파일만 있으면 됨. CMS를 빼고 빌드한 노드는 `node_script_missing`으로 분명히 거절됨 | 위 |
| 비로그인 ssh에서 `$CUBRID` 찾기 | 지금 구현을 그대로 씀. 첫 접속(S2)에서 로그인 셸로 `$CUBRID`를 읽어(bash·sh는 `bash -l`, csh·tcsh는 그 셸로 값을 읽음: 지금의 `node_env_bootstrap`) 그 노드의 스크립트 경로를 정함. `ha_node.sh`는 시작할 때 자기 위치에서 `$CUBRID`를 정하고, `$CUBRID_DATABASES`·`LD_LIBRARY_PATH`는 같은 방법으로 로그인 환경에서 읽음 | `ha_add_nodes.sh`의 `node_env_bootstrap`, `on_node` |
| 지금 원격 임시 파일을 어떻게 실행하는지 | `f=$(mktemp) && cat >"$f" && bash -l "$f" node …`: 파일을 실행 파일로 띄우지 않고 `bash`가 읽음. 그래서 /tmp가 noexec여도 영향 없음. 설치형으로 바뀌면 임시 파일 자체가 없어짐 | `on_node` |

보안상 나아지는 점

- 원격에 코드를 쓰고 실행하는 일이 없음(임시 파일 생성·실행 없음)
- 실행되는 것이 패키지가 설치한 고정 경로 파일이라, 고객이 sshd `ForceCommand`나 `authorized_keys`의 `command=`, sudo 규칙 같은 허용 목록에 넣을 수 있음
- 받은 코드를 쓴 뒤 실행하기 전에 바꿔치기될 틈(임시 파일)이 없음. 남는 틈은 설치 파일 자체이고 3.6이 권한·해시로 줄임

패키징·설치 변경

- `server/scripts/ha_node.sh`, `ha_askpass.sh`를 더하고 `install(PROGRAMS … DESTINATION ${CMS_SHAREDIR}/cms)`에 넣음
- 빌드 때 두 스크립트의 sha256을 계산해 헤더를 만들고 cub_manager에 상수로 넣음(3.6)
- `cm_ssh` 대상, libssh2·OpenSSL 빌드, 고지 설치를 뺌

### 3.5 ssh 원인별 오류

`plan`에서 거르는 것은 C3(CMS의 로그인)·S1·S2에서 나옴. 오류마다 `error`(코드), `message`(노드 이름 + 원인), `action`(사용자 할 일 한 줄)을 상태 응답과 `plan` 결과에 넣고, 노드별 결과는 `node_checks[]`(`node`, `code`, `message`, `action`)로 줌.

| 코드 | 원인 | 알아내는 방법 | 단계 | 사용자 할 일 |
|---|---|---|---|---|
| `local_ssh_missing` | L에 OpenSSH 클라이언트가 없음. `detail`에 없는 것(`ssh`, `ssh-keyscan`, `ssh-keygen`, `setsid`) | 실행 파일 확인 | C3·`ha_check_ssh` | 이 서버에 openssh-clients(배포판 이름) 설치 |
| `name_unresolved` | 노드 이름을 풀 수 없음 | `getent hosts` | C3 | /etc/hosts 또는 DNS에 노드 이름 등록 |
| `port_refused` | ssh 포트 연결 거부 | `ssh-keyscan`·`ssh` 출력("Connection refused") | C3 | 그 노드의 sshd 실행 여부와 포트 번호 확인 |
| `port_timeout` | ssh 포트 연결 시간 초과 | `ConnectTimeout=10` 초과("timed out") | C3 | 방화벽·망 경로 확인 |
| `hostkey_mismatch` | 호스트 키가 화면에서 확인한 지문과 다름 | `ssh-keyscan` 지문 비교 | C3 | 그 노드의 키가 바뀐 이유 확인 후 지문을 다시 확인 |
| `no_password_auth` | sshd가 password·keyboard-interactive를 모두 허용하지 않음 | 인증 없는 접속이 알려 주는 허용 목록 | C3 | 그 노드 sshd의 `PasswordAuthentication` 또는 `KbdInteractiveAuthentication` 허용 |
| `auth_failed` | 비밀번호가 틀림(또는 계정 잠김). 다시 시도하지 않음 | 로그인 실패("Permission denied") | C3 | 계정·비밀번호 확인. 잠김(faillock)이면 관리자에게 해제 요청 |
| `remote_shell_failed` | 원격에서 sh·bash를 실행할 수 없음(로그인 셸이 nologin 등) | 로그인 뒤 첫 명령의 종료 코드 126·127 또는 출력 없음 | S2 | 그 계정의 로그인 셸 확인 |
| `cubrid_not_found` | 그 계정에 CUBRID 환경이 없음 | 로그인 환경의 `$CUBRID`가 비었거나 `cubrid_rel` 없음 | S2 | CUBRID를 설치한 계정으로 다시 입력 |
| `node_script_missing` | 노드 스크립트가 없음 | 종료 코드 127 / 파일 없음 | S2 | 그 노드의 CUBRID 패키지(CMS 포함) 확인 |
| `node_script_version_mismatch` | 노드 스크립트 버전이 L과 다름 | `ha_node.sh version` | S2 | 같은 CUBRID 패키지로 맞춤 |
| `node_script_tampered` | 노드 스크립트 sha256이 L의 기준값과 다름(버전은 같음) | 3.6 | S2 | 패키지 파일로 되돌리고 바뀐 경위 확인 |
| `node_script_unsafe` | 노드 스크립트나 그 폴더를 다른 사용자가 쓸 수 있음 | 3.6 | S2 | 소유자·권한을 설치 상태로 되돌림 |
| `version_mismatch` | CUBRID 전체 버전이 다름 | `ha_node.sh version` | S2 | 모든 노드를 같은 버전으로 맞춤 |
| `controlpath_too_long` | ControlPath가 소켓 경로 길이를 넘음 | 길이 계산 | C3 | `$CUBRID`가 짧은 경로가 되게 설치(드묾) |
| `connection_lost` | 작업 도중 그 노드의 ControlMaster가 끝남(망 끊김, 재부팅) | 명령 종료 코드 255 + `-O check` 실패 | S4~S21 | 2.1대로 되돌림. 노드가 살아나면 비밀번호를 다시 넣고 `rollback` |
| `no_answer` | 작업 도중 바로 끝나는 명령이 시간 안에 답하지 않음(지금과 같음) | 제한 시간 | S4~S21 | 같음 |
| `no_response` | 오래 걸리는 명령이 시간 안에 끝나지 않음(지금과 같음, `rollback_blocked`) | 제한 시간 | S9~S18 | 그 명령이 끝났는지 노드에서 확인 후 `rollback`(R3) |

- `unreachable`은 v5에서 `port_refused`·`port_timeout`·`name_unresolved`로 나뉘어 없어짐
- 작업 중 연결이 끝난 노드는 상태 응답 `lost_nodes[]`(호스트 이름)로 줌. `rollback`에는 `unrestored[]`의 `node`에 나오는 노드(되돌릴 줄이 남은 노드, 끊긴 노드 포함) 모두의 비밀번호가 필요함. 작업이 끝날 때 ControlMaster를 모두 닫으므로(`-O exit`) 끊기지 않은 노드도 다시 로그인해야 하기 때문임. `rollback_failed`로 끝나도 같은 방법으로 `rollback`을 다시 부르면 끝난 줄은 건너뛰고 남은 줄만 함
- 같은 표를 실패 목록표에 넣음(2.15)

### 3.6 스크립트 위조 확인(sha256)

기준 값

- 빌드할 때 `ha_add_nodes.sh`, `ha_node.sh`, `ha_askpass.sh`의 sha256을 계산해 cub_manager에 상수로 넣음(CMake가 헤더를 생성)
- 같은 서버의 파일(`SHA256SUMS` 등)에 두지 않음. 스크립트를 바꿀 수 있는 사람이 그 파일도 같이 바꿀 수 있기 때문임

| 무엇을 | 어디서 | 언제 | 다르면 |
|---|---|---|---|
| `ha_add_nodes.sh`, `ha_askpass.sh`(L) | L의 CMS | C4 직전(스크립트를 띄우기 전), C3 직전(askpass) | `script_tampered`로 거절 |
| 각 노드의 `ha_node.sh` | L의 스크립트가 `ssh`로 그 노드의 `sha256sum` 결과를 읽어 CMS가 넘긴 기준값과 비교 | S2 | `node_script_tampered`(버전이 다르면 `node_script_version_mismatch`) + 노드 이름 |
| 각 노드의 `ha_askpass.sh` | 쓰지 않음(askpass는 L에서만 실행) | — | — |
| 백업 파일 전송(S10) | 받는 노드의 `ha_node.sh recv`가 크기·sha256 비교(지금은 헬퍼가 원격 `sha256sum`으로 비교) | S10 끝 | `transfer_failed` |
| 파일 권한 | L은 CMS, 노드는 S2 | 위와 같은 때 | 스크립트와 그 폴더(`share/cms`, `share`)가 설치 계정 소유가 아니거나 그룹·다른 사용자 쓰기가 있으면 `script_unsafe` / `node_script_unsafe` |

- 기준값은 CMS가 스크립트 인자로 넘김(`--node-script-sha256 <값>`). 스크립트가 바뀌었으면 C4에서 이미 거절됐으므로 넘긴 값을 믿을 수 있음
- `sha256sum`은 coreutils에 들어 있어 대상 OS 모두에 있음(OS 11개 실측에서 원격 `sha256sum` 비교가 모두 됨. 대상 목록이 확정되면 그 목록과 다시 맞춤)

한계

- cub_manager 바이너리까지 바꿀 수 있는 공격자는 막지 못함(서명이 아니라 바이너리에 넣은 해시라서). 그 수준은 패키지 서명이나 OS 파일 무결성 도구(AIDE, IMA 등)의 영역임
- 확인과 실행 사이의 틈은 권한 확인으로 줄일 뿐 0이 아님(같은 계정이나 root는 그 사이에 바꿀 수 있음)
- 원격 노드의 확인도 그 노드의 `sha256sum`과 셸을 믿는 것임

### 3.7 바뀌는 설계 절·단계와 작업량

| 절·단계 | 바뀌는 것 |
|---|---|
| 0·13 | 대상 OS 범위(지원 중인 OS, OpenSSH 8.0 이상). 13절 FIPS 한계 삭제 |
| 1.1 | 구성 요소: `cm_ssh` → OpenSSH 클라이언트 + `ha_node.sh` + `ha_askpass.sh` |
| 3.2 | CMS → 헬퍼 fd 3 대신 FIFO, 스크립트 인자에 ControlPath 목록과 노드 스크립트 기준 해시 |
| 4 | 4.1 표에 3.1 비교 추가, 4.2 경로를 3.1 흐름으로 |
| 5 | 전면 교체(헬퍼 → OpenSSH 사용 규칙, 3.2 표) |
| 6.1 C3 | `cms_agent` → `cms_login`: 노드마다 키 확인·인증 방법 확인·ControlMaster 로그인, 노드별 결과(`node_checks[]`). C4 앞에 스크립트 해시·권한 확인 |
| 6.2 S2 | 노드 스크립트 버전·해시·권한 확인 추가 |
| 7 | P2(헬퍼 준비 → 로그인 완료), P5(헬퍼 종료 → `-O exit`·ControlPersist), P9(rollback은 C3 다시), P13(헬퍼 소켓 → ControlPath 이름을 실행마다 새로) |
| 9.1 | 원격 실행 규칙: 임시 파일 대신 설치 경로 + 하위 명령 |
| 10 | `ha_check_ssh` 내부, 오류 코드(3.5), `node_checks[]` |
| 실패 목록표 | 2.1·2.3 코드 교체, 2.15 원인별 오류 표 |

대략적인 작업량(1인 기준)

| 일 | 양 |
|---|---|
| CMS C3 다시 쓰기(keyscan·인증 방법 확인·FIFO·ControlMaster·오류 코드), `ha_check_ssh` | 1.5일 |
| `ha_node.sh` 분리(노드 동작 이동, `recv`·`version`·`backup`, 범용 통로 삭제), `ha_add_nodes.sh`의 `on_node`·전송 바꾸기 | 1.5일 |
| sha256 상수 생성(CMake), 권한 확인, 설치 규칙, `cm_ssh`·libssh2·OpenSSL 빌드 삭제 | 0.5일 |
| 시험: 2노드 회귀 + OS 실측 목록에서 OpenSSH 판별 확인(askpass 조건, 키 종류, 인증 방법 확인, ControlPersist, 이어 보내기) | 1.5일 |
| 설계·실패 목록표 정리 | 0.5일 |
| 합계 | 약 5.5일 |

실측으로 확인할 것(OpenSSH 8.0~9.x)

- askpass: 8.0에서 `DISPLAY=x` + `setsid`, 8.4 이상에서 `SSH_ASKPASS_REQUIRE=force`로 askpass가 불리는지. keyboard-interactive 질문에도 askpass가 쓰이는지
- 인증 없는 접속이 허용 방법 목록을 내는지(판마다 출력 문구)
- 판마다 먼저 고르는 호스트 키 종류, `HostKeyAlgorithms` 고정과 작업 전용 `known_hosts`로 지문 고정이 되는지
- `NumberOfPasswordPrompts=1` + 방법 하나로 틀린 비밀번호가 실패 한 번(btmp 한 줄)으로 끝나는지
- ControlPersist 시간이 지나면 마스터가 끝나는지, `ServerAliveInterval`로 망 끊김이 60초 안에 드러나는지
- `ssh`가 3 이상 fd를 닫는지(3.1 (c) 판단 근거)

## 부록 4. 백업 원본 고르기(케이스 3·4 선택, 케이스 5·6과 공유) — 설계 차이 (검토 중, 코드는 아직 v4)

케이스 3·4에 "백업 원본: 마스터(기본) / 기존 슬레이브 / 기존 레플리카 지정"을 붙일 때 v4와 달라지는 것. 열지 말지는 슬레이브 원본 데이터 검증 결과로 정하고, 통과하면 케이스 5·6도 같은 경로로 엶. 아래에서 B = 백업 원본 노드(B ≠ M).

### 4.1 엔진에서 확인한 것

| 사실 | 근거 | 설계에 주는 것 |
|---|---|---|
| `restoreslave -s master`는 복원한 DB의 `db_ha_apply_info`를 모두 지우고 백업 시점 LSA로 한 줄을 새로 넣음 | `util_sa.c` restoreslave(`init_ha_catalog`), `delete_all_ha_apply_info`, `insert_ha_apply_info` | 지금(v4)의 경로 |
| `-s slave`·`-s replica`는 새 줄을 넣지 않음. 복원한 DB(=B의 백업)에 있던 줄 중 `copied_log_path`가 "이 노드의 `realpath(ha_copy_log_base 또는 $CUBRID_DATABASES)`/<DB>_<마스터>"와 다른 줄만 지우고 그 줄 하나를 남김 | `util_sa.c` `delete_all_slave_ha_apply_info`(`DELETE … WHERE db_name = ? and copied_log_path <> ?`) | 새 노드의 복제 로그 경로가 B와 글자 그대로 같아야 B의 반영 위치가 살아남음. 다르면 그 줄도 지워져 새 노드의 반영 시작점이 없어짐(데이터 빠짐·중복 위험). → B 원본이면 새 노드의 복제 로그 위치를 B와 같게 강제하거나, 다르면 `plan`에서 거절 |
| 엔진의 `ha_make_slavedb.sh`도 원본(target)에서 `backupdb -C`, 복사, `restoreslave -s <원본 상태> -m <마스터>`, 마스터에서 `copylogdb --start-page-id=-1`(마스터에 남은 로그 전체) 순서임. 원본으로 slave·replica를 받음 | `contrib/scripts/ha/ha_make_slavedb.sh`(`step_func_slave_from_replica` 등, `restore_db_to_current`, `copy_active_log_from_master`) | 같은 순서를 따름. 엔진 절차와 어긋나지 않음 |
| 첫 로그 복사는 원본과 상관없이 M에서 받음. 새 노드는 B가 반영한 위치(남은 줄)부터 M의 로그를 반영함 | 위 | B의 반영 지연만큼의 M 로그가 M에 남아 있어야 함(4.4) |

### 4.2 단계별 차이

| 단계 | v4(마스터 원본) | B 원본 |
|---|---|---|
| S2 | M·E·X·Y 확인 | + B가 D 모두 standby인지, B의 반영 지연(`cubrid applyinfo`로 M 로그 대비 B 반영 위치), B와 새 노드의 복제 로그 경로(4.1) |
| S3 사전 점검 | L 백업 폴더, 새 노드 받는 폴더 | + B의 백업 폴더 쓰기·여유(D 전체 백업 크기). L 백업 폴더 점검은 빠짐(L=M이 원본이 아니면) |
| S9 backup | L에서 `backupdb -C` | B에서 원격 `backupdb -C -D <B 백업 폴더>`(노드 스크립트 `backup` 하위 명령). 백업 폴더 지우기 되돌리기는 B에 기록 |
| S10 send_backup | L → 새 노드 | B → L → 새 노드 중계. 중간 파일 없이 흘려 보냄: v5면 `ssh B 'cat <파일>' | ssh X 'ha_node.sh recv …'`, v4(cm_ssh)면 `exec B cat`의 출력을 `exec X --stdin`으로 넘김. B에서 `sha256sum`과 크기를 먼저 읽고 X의 `recv`가 비교. 끊기면 받은 크기부터 다시(B 쪽은 `tail -c +N`) |
| S13 restore | `restoreslave -s master -m M` | `restoreslave -s slave|replica -m M`(케이스 5 경로와 공유). 새 노드의 복제 로그 위치를 B와 같게 맞춘 뒤(S11) 실행 |
| S14 first_copylog | M에서 `--start-page-id=-1` | 같음 |
| S15~S21 | | 같음. S21에서 B의 백업 폴더도 지움 |

- 단계 번호·수: 새 단계는 없음. S9의 노드가 L에서 B로 바뀌고, S3에 B 점검이 하나 늘어남(노드 하나 = 단계 하나). 단계 id는 그대로 두고 `node`가 B가 됨
- 마스터 NIC: L이 M이면 백업 데이터가 B → M → X로 M을 지나가므로 M의 NIC를 지나는 양은 줄지 않음(들어오고 나가서 오히려 두 배). 줄어드는 것은 M의 백업 읽기 I/O와 백업 시간 동안 M에 걸리는 부하뿐임. NIC까지 줄이려면 B → X 직접 전송이 필요한데, 이는 노드끼리 ssh 자격 증명이 필요해 이번 범위 밖임

### 4.3 `plan`의 원본 고르기

- 기본은 M. 요청 키 `backup_source`(노드 이름)로 B를 지정. N = 0(슬레이브 없음)이고 레플리카도 고르지 않으면 M
- B 조건: E 안의 노드, D 모두 standby, 복제 로그 경로가 새 노드와 같게 맞출 수 있음(4.1), 백업 폴더 자리
- `plan`이 노드별로 보여 줄 것: 상태(standby 여부), 반영 지연(페이지·시간), 백업 폴더 여유. 지연이 가장 작은 standby 슬레이브를 추천 값으로 표시만 함(자동으로 고르지 않음)
- 지연이 크면 경고: B가 반영한 위치가 오래될수록, 새 노드가 첫 복사 때 M에서 받아야 할 로그가 많아지고, 그 사이 M이 보관 로그를 지우면 S14가 실패함. 부록 1의 보완 항목(M의 `log_max_archives`·`force_remove_log_archives`, 보관 로그 생성 간격으로 위험 어림)과 하나로 묶어 "B 반영 위치가 M에 남은 가장 오래된 보관 로그보다 앞이면 거절, 가까우면 경고"로 함
- 레플리카 원본: 레플리카는 보통 `force_remove_log_archives=yes`(그 로그를 받아 가는 노드가 없음)임. 백업이 오래 걸리면 백업 중에 생긴 레플리카 자신의 보관 로그가 지워져 백업이 실패할 수 있음[실측 항목]. `plan`이 B의 이 값을 보여 주고 큰 DB면 경고

### 4.4 되돌리기·취소

| 상황 | 동작 |
|---|---|
| S9 중 취소 | 지금 로컬 S9와 같은 규칙: B의 `backupdb`가 끝난 뒤 되돌리기(클라이언트를 끝내도 서버 쪽 백업이 바로 멈추는지 확인되지 않음, 9.3). 제한 시간을 넘으면 `no_response` → `rollback_blocked` |
| S9·S10 실패 | B의 백업 폴더(작업 uuid 폴더)를 지우는 되돌리기를 S9 직전에 B 앞으로 기록. 받은 폴더는 지금처럼 새 노드에서 지움 |
| S10 중 B 연결 끊김 | 중계가 끊김 → 2.1의 S10 규칙(이어 보내기 최대 3번). B에 다시 붙지 못하면 `fail`, B의 백업 폴더는 `unrestored`에 남음 |
| 성공 | S21에서 B의 백업 폴더도 지움 |

- B의 설정·DB는 바꾸지 않음(백업 읽기만). 그래서 B 쪽 되돌리기는 백업 폴더뿐임

### 4.5 의존성 표(7절)에 더할 것

| no | 앞 | 뒤 | 맞추는 방법 |
|---|---|---|---|
| P14 | B의 반영 위치(S9 백업 시점) | S14 첫 로그 복사 | M에 그 위치 이후의 로그가 남아 있어야 함. `plan` 확인 + S14 실패 문구 |
| P15 | S11의 새 노드 복제 로그 위치 | S13 `restoreslave -s slave|replica` | 새 노드의 `realpath` 경로가 B와 같아야 함. S2에서 확인, S11에서 같게 씀 |
| P16 | S9(B 원격 백업) | S10 중계 | B의 `backupdb`가 끝나고 `sha256sum`·크기를 읽은 뒤 전송 |

### 4.6 작업량(1인 기준)

| 일 | 양 |
|---|---|
| S9 원격 백업(노드 스크립트 `backup`), B 백업 폴더 되돌리기·정리 | 0.5일 |
| S10 중계 전송(흘려 보내기, 크기·sha256, 이어 보내기) | 1일 |
| S2·S3 B 확인(standby, 지연, 경로 같음, 자리), `plan`의 원본 정보·추천·경고 | 1일 |
| S13 `-s slave|replica`, S11 복제 로그 위치 맞추기 | 0.5일 |
| 시험: 슬레이브·레플리카 원본 각각, 지연이 있을 때, 경로가 다를 때 거절, 취소·실패 | 1.5일 |
| 설계·실패 목록표 | 0.5일 |
| 합계 | 약 5일(v5 OpenSSH 방식 위에서 하면 중계가 단순해져 0.5일 줄어듦) |

실측으로 확인할 것

- 슬레이브·레플리카 원본으로 만든 새 노드의 데이터가 M과 같은지(백업 중·백업 뒤 쓴 행 포함). 이 결과로 옵션을 열지 정함
- 새 노드의 복제 로그 경로가 B와 다를 때 `-s slave` 복원 뒤 `db_ha_apply_info`가 비는지(4.1 소스 판단의 실측)
- 레플리카 원본에서 백업이 오래 걸릴 때 보관 로그 삭제로 백업이 실패하는지
- B의 반영 지연이 클 때 S14가 M의 보관 로그 삭제로 실패하는 지점

### 3.8 검토 반영 — 구현 기준 (3.1~3.7과 다르면 이 절이 우선)

v5 설계 검토(인증 소켓 수명, C3 막힘, 다시 로그인 불가, 노드 스크립트 인자, ssh 설정 격리, 오류 분류, 해시 확인 틈, ControlPath 폴더)를 반영한 규칙임. 결정: 작업 중 끊긴 노드에 다시 로그인하지 않는 것은 받아들임(되돌리기 때 비밀번호 다시 입력이 기본 흐름).

#### 3.8.1 공통 규칙 (CMS·스크립트)

| 규칙 | 내용 |
|---|---|
| 설정 격리 | 모든 ssh 호출은 `-F <작업 폴더>/ssh_config.<실행 번호>`. 계정의 `~/.ssh/config`는 읽지 않고, 파일 끝 `Include /etc/ssh/ssh_config`로 OS 설정(RHEL crypto-policies)은 유지. ssh 설정은 먼저 나온 값이 이기므로 작업 값이 OS 값보다 앞섬. `-F /dev/null`은 쓰지 않음 |
| 공통 값 | `ControlMaster no`, `BatchMode yes`, `PubkeyAuthentication no`, `GSSAPIAuthentication no`, `HostbasedAuthentication no`, `PasswordAuthentication no`, `KbdInteractiveAuthentication no`, `ProxyJump none`, `StrictHostKeyChecking yes`, `GlobalKnownHostsFile /dev/null`, `UserKnownHostsFile <작업 known_hosts>`, `UpdateHostKeys no`, `CheckHostIP no`, `ConnectTimeout 10`, `ServerAliveInterval 15`, `ServerAliveCountMax 20`, `ForwardAgent no`, `ForwardX11 no`, `ClearAllForwardings yes` |
| 노드마다 | `Host <요청의 host>` 블록에 `HostName`, `Port`, `User`, `ControlPath`, `HostKeyAlgorithms`. C3와 스크립트가 같은 블록을 씀(User·Port가 어긋날 일 없음) |
| 인증 없는 호출 | 스크립트 쪽 ssh는 인증 방법이 모두 꺼져 있음. 마스터가 없을 때 보통 접속으로 넘어가도 인증 시도 없이 255로 끝남(계정 잠금 횟수가 오르지 않음). 그리고 호출 전마다 `-O check` |
| 마스터 수명 | `ControlPersist=yes`(시간 제한 없음). 정리는 S21 끝·감시 셸·CMS(3.8.2) |
| ControlPath 폴더 | 실행마다 새 폴더 `$CUBRID/tmp/hs<임의 8자>`(700, 이미 있으면 실패), 소켓 이름은 노드 순번. `rollback`·`cleanup`도 새 폴더. 새 로그인 전에 옛 실행 폴더의 마스터에 `-O exit` |
| 끊김 판단 | ServerAlive 15초 × 20 = 약 5분 무응답이면 마스터가 끝남. 다시 로그인하지 않음 |

#### 3.8.2 CMS 쪽 (C3·C4·`ha_check_ssh`)

요청 시간 제한(확인한 사실)

| 구간 | 제한 | 근거 |
|---|---|---|
| 브라우저 → NCA | 60초(기본값) | NCA apiClient 기본 timeout |
| NCA → CMS `ha_add_nodes` | 없음(`ha_check_ssh`만 40초) | NCA ha.service가 timeoutMs 없이 부름 |
| CMS | 연결당 360초 | cm_httpd.cpp `DEFAULT_HTTP_TIMEOUT` |
| CMS 동시 처리 | 요청 처리 스레드 22개가 각자 받음. `ha_*` task는 받은 스레드에서 끝까지 돎 | cm_httpd.cpp, cm_server_extend_interface.cpp |

- 그래서 C3는 요청 안에서 노드끼리 병렬로 로그인하고 전체를 30초 안에 끝냄(넘으면 남은 노드는 `port_timeout`)

C3 `cms_login`(노드마다, 노드끼리는 병렬)

| 차례 | 하는 일 | 제한 | 실패 코드 |
|---|---|---|---|
| 0 | L의 `ssh`·`ssh-keyscan`·`ssh-keygen`·`setsid` 확인, `ha_askpass.sh` 해시·권한 확인 | — | `local_ssh_missing`, `script_tampered`, `script_unsafe` |
| 1 | 새 ControlPath 폴더, 길이 확인, 옛 실행 폴더 마스터 `-O exit` | — | `controlpath_too_long` |
| 2 | 이름 풀기 | — | `name_unresolved` |
| 3 | `ssh-keyscan -T 10`, 화면 지문과 같은 키 한 줄만 작업 `known_hosts`에, 그 키 종류로 `HostKeyAlgorithms` 고정. 키를 못 받으면 포트에 직접 접속해 거부·시간 초과를 가림 | 12초 | `port_refused`, `port_timeout`, `hostkey_mismatch` |
| 4 | 허용 인증 방법 확인(`PreferredAuthentications=none`, `BatchMode=yes`). password가 있으면 그것, 없으면 keyboard-interactive 하나만 | 12초 | `no_password_auth` |
| 5 | FIFO(`<작업 폴더>/pw.<임의 16자>`, 0600), `ssh -M -N -f`를 제어 터미널 없이(`SSH_ASKPASS=ha_askpass.sh`, `SSH_ASKPASS_REQUIRE=force`, `DISPLAY=x`, `HA_ASKPASS_FIFO`). 명령줄 `-o`로 `ControlMaster=yes`, `ControlPersist=yes`, `BatchMode=no`, 고른 방법 하나, `NumberOfPasswordPrompts=1`만 덮어씀 | — | — |
| 6 | FIFO를 `O_WRONLY|O_NONBLOCK`으로 엶(ENXIO면 기한 안에서 다시). 붙으면 비밀번호를 한 번 쓰고 닫음. ssh가 askpass를 부르지 않고 끝나면 기다리지 않고 원인을 가림. 기한 넘으면 ssh 프로세스 그룹을 끝냄 | 20초 | `auth_failed`, `port_timeout`, `ssh_failed` 등 |
| 7 | 비밀번호를 준 뒤 askpass가 한 번 더 붙으면(일회용 코드, 만료 비밀번호 변경) 답하지 않고 ssh를 끝냄(FIFO는 ssh가 끝날 때까지 남겨 둠) | — | `auth_failed`(문구에 "비밀번호 말고 다른 것을 물음") |
| 8 | FIFO 삭제, 메모리의 비밀번호 지움, `-O check` | 5초 | `auth_failed` |

- 한 노드가 실패해도 다른 노드는 끝까지 확인하고, 하나라도 실패하면 로그인한 노드에 `-O exit` 한 뒤 거절. 실패한 노드에 다시 시도하지 않음(노드당 로그인 시도 한 번)
- C3가 실패하면 스크립트를 띄우지 않고 `ha_add_nodes` 응답 자체가 failure(`note`, `error`, `message`, `action`, `node_checks[]`). `plan`·`run`은 작업 폴더를 남기지 않고 `rollback`·`cleanup`은 옛 작업 폴더를 건드리지 않음

C4 앞 확인과 실행

| 무엇을 | 방법 | 다르면 |
|---|---|---|
| `ha_add_nodes.sh`·`ha_node.sh`·`ha_askpass.sh`·`ha_watch.sh` sha256 | cub_manager 상수(CMake가 server/scripts에서 헤더 생성)와 비교 | `script_tampered` |
| 소유자·권한 | 파일과 `/`까지 모든 상위 폴더가 CMS 계정 또는 root 소유, 그룹·다른 사용자 쓰기 없음 | `script_unsafe` |
| 확인과 실행 사이 틈 | `ha_add_nodes.sh`는 CMS가 연 fd로 해시하고 그 fd(`/dev/fd/3`)로 실행. 스크립트는 `$0`으로 자기 위치를 찾지 않음(`$CUBRID/share/cms` 고정) | — |

감시 셸 `ha_watch.sh`(스크립트 쪽이 작성, CMS가 띄움)

- CMS가 `setsid ha_watch.sh --ssh-config <파일> --cp-dir <hs 폴더> -- <스크립트 인자>`를 fd 3에 `ha_add_nodes.sh`를 연 채로 띄움
- 감시 셸: TERM·INT·HUP 무시 → `bash /dev/fd/3 "$@" &`(스크립트는 새 프로세스, 취소 신호는 지금처럼 job 파일의 스크립트 pid로만) → `wait` → 끝나면(SIGKILL 포함) 설정 파일의 `Host` 목록마다 `ssh -F <파일> -O exit <host>`, hs 폴더 삭제 → 끝
- 감시 셸까지 죽은 경우: CMS가 상태 조회에서 `interrupted`를 볼 때, `rollback`·`cleanup`을 받을 때 작업 폴더에 적힌 옛 hs 폴더의 마스터에 `-O exit`

#### 3.8.3 스크립트 쪽

원격 실행(`on_node`)

- 명령 문자열은 고정: `ssh -F "$SSH_CONFIG" -T <host> '<노드의 $CUBRID>/share/cms/ha_node.sh'`. 하위 명령과 인자는 표준 입력 앞부분에 보냄: 첫 값은 인자 개수, 그다음 인자마다 NUL로 끝남. `recv`의 파일 내용은 그 뒤에 이어짐. 원격 로그인 셸(csh·tcsh 포함)이 인자를 해석하지 않음
- 노드의 `$CUBRID`는 S2 첫 확인(로그인 셸로 읽은 값, 3.4)으로 정함
- 호출 전 `-O check`. 실패면 그 호출은 하지 않고 바로 `connection_lost`
- 결과는 지금처럼 `@@BEGIN` / `@@END <종료 코드>` 사이만 읽음. 표준 오류는 따로 받아 마지막 한 줄을 오류 문구로 씀
- 스크립트 본문 전체를 `main() { … }; main "$@"; exit $?`로 감쌈(bash는 실행 중 파일을 조금씩 읽으므로, 실행 중 같은 inode가 바뀌어도 영향이 없게). `ha_node.sh`도 같음

결과 분류

| 관찰 | 코드 | 되돌리기 |
|---|---|---|
| `@@END n` 있음 | 노드 동작의 결과(n = 0이면 성공) | 실패면 보통 되돌리기 |
| 표시 없음 + ssh 255 + `-O check` 실패 | `connection_lost` | 그 명령이 오래 걸리거나 CUBRID 프로세스를 띄우는 것(restore, first_copylog, backup, heartbeat·server·cub_master, reload, repl)이면 `rollback_blocked`(노드에서 아직 돌 수 있음). 아니면 되돌리기, 그 노드 줄은 `unrestored`(다시 로그인하지 않으므로) |
| 표시 없음 + ssh 255 + `-O check` 성공 | `ssh_failed` + 표준 오류 마지막 줄(채널 열기 실패, MaxSessions 등) | 보통 되돌리기 |
| 표시 없음 + 다른 종료 코드 | `ssh_failed` + 표준 오류 마지막 줄 | 보통 되돌리기 |
| 로컬 제한 시간 초과 | 지금과 같음(`no_response` / `no_answer`) | 2.1·2.6 |

S2 노드 확인 순서(127 겹침 해소)

1. 셸 확인: `ssh … -T <host> true`. 0이면 다음. "This account is currently not available"(종료 코드 1)이면 `account_no_shell`, 126·127이면 `remote_shell_failed`
2. `$CUBRID` 읽기: 로그인 셸로 값 읽기(3.4). 비면 `cubrid_not_found`
3. `ha_node.sh version`: 127이면 `node_script_missing`. `SCRIPT_VERSION`(스크립트 버전 + 빌드 식별자)이 다르면 `node_script_version_mismatch`, 같은데 `SELF_SHA256`이 CMS 기준값과 다르면 `node_script_tampered`, `OWNER`·`MODE`(파일과 `/`까지 상위 폴더)가 느슨하면 `node_script_unsafe`, `CUBRID_VERSION`이 다르면 `version_mismatch`
4. 결과를 `node_checks` 파일의 그 노드 줄에 씀(임시 파일 + mv)

- 접속 횟수: S2 확인은 C3가 연 마스터 위에서 하므로 새 TCP 접속·인증이 늘지 않음. `ha_check_ssh`와 C3의 keyscan 중복은 남음(fail2ban은 [하])

`ha_node.sh` 인자 허용 목록(노드 안에서 확인, 어기면 그 하위 명령은 아무것도 하지 않고 실패)

| 대상 | 허용 |
|---|---|
| 작업 번호 | 모든 호출의 첫 인자 `--job <uuid>`(16진수). 아래 "작업 표시"는 이 값으로 만듦 |
| 설정 파일(`conf_edit`, `undo_file`, `drop_backups`, `dbtxt_*`) | `$CUBRID/conf/cubrid.conf`, `$CUBRID/conf/cubrid_ha.conf`, `$CUBRID_DATABASES/databases.txt`와 그 `.bak.<uuid>.<번호>`·`.new.<uuid>.<번호>`뿐. `$CUBRID`·`$CUBRID_DATABASES`는 노드 자신의 로그인 환경 값 |
| 절·키·값 | 절은 `[common]` 또는 `[@<DB 이름>]`, 키는 `[a-z_]+`, 값은 `[A-Za-z0-9@:,._/-]*` |
| `recv`, 받는 폴더 `mkdir` | `<받는 폴더>/ha_add_nodes_backup/<uuid>/` 아래(`..` 없음, 실제 경로 확인) |
| `move_back`, `remove`, `clear`, `rmdir_empty` | 작업 표시가 있는 경로(`*.ha_add_nodes_<uuid>`, `…/ha_add_nodes_backup/<uuid>…`), 또는 `ha_node.sh`가 노드의 `databases.txt`·`cubrid_ha.conf`에서 직접 계산한 D의 볼륨·로그 폴더와 복제 로그 폴더(`<복제 로그 위치>/<D>_<노드>`). `rmdir_empty`는 받는 폴더와 복제 로그 위치도 됨(비었을 때만 지워짐) |
| `keep_old` | D 이름과 HA 노드 이름만 받고 경로는 노드가 계산 |
| 범용 실행 | 없음 |

오류 문구

- `auth_failed`: "비밀번호가 틀렸거나, 계정이 잠겼거나(faillock), 로그인이 허용되지 않은 계정(AllowUsers·DenyUsers, pam_access)이거나, 비밀번호가 만료됐거나, 인증 시도 횟수 제한(MaxAuthTries)에 걸렸음". 할 일: "비밀번호를 확인하고, 맞다면 그 노드 관리자에게 계정 잠김·접속 허용 여부를 확인"
- `ssh_failed`: 다른 코드에 들지 않는 ssh 실패. 문구에 ssh 표준 오류 원문 한 줄(예: 키 교환·암호 협상 실패, No route to host, Network is unreachable, Connection reset by peer, Too many authentication failures)
- `account_no_shell`: 그 계정의 로그인 셸이 로그인을 막음(nologin). 할 일: 로그인 셸 확인

#### 3.8.4 실패 목록표에 바뀌는 것

- 2.1의 "되돌리기에서 그 노드에 다시 접속(헬퍼가 새 세션)"은 v5에서 성립하지 않음: 끊긴 노드는 그 실행에서 다시 접속하지 않고 `unrestored`로 남김. 사용자가 비밀번호를 다시 넣어 `rollback`(화면 기본 흐름)
- S13·S14(또는 다른 오래 걸리는 명령) 중 `connection_lost`는 `rollback_blocked`(원격 restoreslave가 계속 돌 수 있음. v4에서 이미 같게 고침)
- 2.15에 `ssh_failed`, `account_no_shell` 추가
