# HA 노드 추가 — 스크립트 + 전용 CMS API 설계 (v2.1)

싱글 DB를 HA로 바꾸고, HA에 슬레이브·레플리카를 여러 대 한 번에 추가하는 기능의 설계임. 동작은 셸 스크립트 하나에 넣고, CMS에는 그 스크립트만 부르는 얇은 전용 API를 둠. runscript는 쓰지 않음.

## 0. 범위와 전제

- 고객사 배포 환경을 기준으로 함. 구성은 마스터:슬레이브:레플리카 = 1:N:M(N, M ≥ 0)임. 시험용 1:1:1 구성을 기준으로 하지 않음.
- 각 서버에 대해 주어지는 것은 OS 계정 아이디·비밀번호뿐임(ssh 키 배포 없음). 원격 작업은 ssh로 함(폐쇄망 방안은 따로 다룸).
- 한 번 실행으로 새 슬레이브 a대와 새 레플리카 b대(a + b ≥ 1)를 같이 추가함.
- 스크립트는 파일 안의 변수를 고쳐 쓰는 방식(예전 ha_make_slavedb.sh 방식)을 쓰지 않음. 필요한 값은 모두 인자로 받음.

| 케이스 | 실행 위치(L) | 시작 상태 | 하는 일 |
|---|---|---|---|
| 1 | 마스터가 될 서버 | 싱글 DB | HA 마스터로 전환 + 새 슬레이브 추가 |
| 2 | 마스터가 될 서버 | 싱글 DB | HA 마스터로 전환 + 새 레플리카 추가 |
| 3 | 마스터 | HA 1:N:M | 새 슬레이브 추가 |
| 4 | 마스터 | HA 1:N:M | 새 레플리카 추가 |
| 5(선택) | 기존 슬레이브 | HA 1:N:M (N ≥ 1) | 그 슬레이브를 백업 원본으로 써서 새 슬레이브·레플리카 추가. 마스터는 원격 |
| 6(선택) | 기존 레플리카 | HA 1:N:M (M ≥ 1) | 그 레플리카를 백업 원본으로 써서 새 슬레이브·레플리카 추가. 마스터는 원격(가능 여부·제약만 적음, 8절) |

- 1·2와 3·4는 같은 흐름임. 새 노드 목록에 슬레이브와 레플리카가 섞여 있어도 됨.
- 실행 대상(위자드를 여는 호스트)의 상태로 케이스가 정해짐: 싱글 DB → 1·2, HA 마스터 → 3·4, HA 슬레이브 → 5, HA 레플리카 → 6. 5·6은 선택 구현이며 같은 우선순위로 나중에 함.
- 기호: L = 스크립트가 도는 노드, M = 현재 마스터, S1..SN = 기존 슬레이브, R1..RM = 기존 레플리카, E = 기존 노드 전부(M, S, R. 케이스 5·6은 L도 포함), X1..Xa = 새 슬레이브, Y1..Yb = 새 레플리카, D1..Dk = 대상 DB(`ha_db_list`).

## 1. 큰 그림

### 1.1 구성 요소와 맡는 일

| 구성 요소 | 맡는 일 | 맡지 않는 일 |
|---|---|---|
| CMS 전용 API | 입력 형식 검사, 작업 폴더 만들기, ssh 헬퍼(에이전트)를 띄워 비밀번호를 넘기기, 스크립트 실행, 진행 상태·로그 읽어 주기, 취소 신호 | HA 판단, 설정 편집, 원격 명령, 비밀번호를 스크립트에 넘기기 |
| 작업 스크립트 `ha_add_nodes.sh` | 확인, 전환, 설정 편집, 백업·전송·복원, HA 시작, 기존 노드 등록, 확인, 되돌리기, 진행 기록 | 비밀번호 다루기(아예 모름) |
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
| 자신의 cubrid_ha.conf | `ha_node_list` = M + S1..SN + X1..Xa, `ha_replica_list` = R1..RM + Y1..Yb | 같음. 자기 이름이 `ha_replica_list`에 있어야 heartbeat가 뜸. 두 목록의 그룹 이름이 같아야 함 |
| `ha_copy_sync_mode` | 값 개수 = `ha_node_list` 노드 수. X가 늘 때마다 마스터·슬레이브 노드에서 값 하나씩 늘림 | 레플리카 노드는 값과 상관없이 항상 async. 레플리카는 `ha_node_list`에 없으므로 다른 노드의 값 개수도 그대로 |
| 이 노드가 로그를 받는 곳 | `ha_node_list`의 다른 노드 전부 | `ha_node_list`의 노드 전부(다른 레플리카에서는 받지 않음) |
| 이 노드의 로그를 받는 곳 | 다른 마스터·슬레이브 전부, 레플리카 전부 | 없음 |
| 기존 노드에서 할 일 | 설정 + reload + `heartbeat repl start <X>` | 설정 + reload만. `repl start <Y>`는 엔진이 거부함(`ha_node_list`에 없는 이름은 "not listed") |
| 복원 | `restoreslave -s <백업 원본의 상태> -m <M>` | 같음 |
| 첫 로그 복사 | M에서 `copylogdb --start-page-id=-1` | 같음 |

- 근거: 엔진이 copylogdb·applylogdb를 띄우는 대상은 `ha_node_list`의 노드뿐임. 매뉴얼의 "운영 중 레플리카 추가" 절차도 기존 노드는 `ha_replica_list`에 넣고 reload만 하며, 레플리카용 복제 프로세스는 띄우지 말라고 함.
- reload는 노드 목록만 다시 읽고 프로세스를 띄우지 않음. 그래서 새 슬레이브는 기존 노드마다 `repl start`가 필요함(이전 POC 실측: 이게 없으면 failover 직후 쓴 데이터가 옛 마스터에서 빠짐).

### 2.2 기존 노드에서 할 일 (1:N:M)

| 기존 노드 | cubrid_ha.conf | databases.txt | reload | repl start |
|---|---|---|---|---|
| M, S1..SN | `ha_node_list` += X1..Xa, `ha_copy_sync_mode` 값 a개 늘림, `ha_replica_list` += Y1..Yb | D마다 db-host += X1..Xa | 1회 | X마다(a회) |
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
- 전환 때 HA로 넣지 않는 DB는 이 방법으로 싱글로 남김. cub_master를 다시 띄우면 그 노드의 DB 서버가 모두 내려가므로, 떠 있던 DB는 전환 중에 멈췄다 다시 뜸(확인을 받음).

## 3. 입력 — 위자드 → CMS → 스크립트

값은 모두 인자로 넘김. 스크립트 파일은 고치지 않음.

### 3.1 위자드 → CMS (요청 JSON)

```
{
  "task": "ha_add_nodes", "action": "run",
  "master_host": "nodeA",
  "db_names": ["db1", "db2"],                  // 싱글일 때만: HA로 넣을 DB
  "confirm_master_restart": "yes", "confirm_other_db_restart": "yes",
  "new_nodes": [
    {"role": "slave",   "host": "nodeX1", "ssh_port": 22, "os_user": "cubrid", "hostkey_sha256": "SHA256:...", "os_password": "..."},
    {"role": "replica", "host": "nodeY1", "ssh_port": 22, "os_user": "cubrid", "hostkey_sha256": "SHA256:...", "os_password": "..."}
  ],
  "existing_nodes": [
    {"host": "nodeS1", "ssh_port": 22, "os_user": "cubrid", "hostkey_sha256": "SHA256:...", "os_password": "..."},
    {"host": "nodeR1", "ssh_port": 22, "os_user": "cubrid", "hostkey_sha256": "SHA256:...", "os_password": "..."}
  ],
  "repl_log_home": "", "backup_path": "", "remote_backup_path": "",
  "rollback_convert": "no", "start_ha": "yes"
}
```

- 노드 원소의 키는 `host`, `ssh_port`, `os_user`, `os_password`, `hostkey_sha256`이고, `new_nodes` 원소에만 `role`(`slave`/`replica`)이 더 있음. 본문의 "포트·계정·지문"은 이 키들을 줄여 부른 것임.
- `existing_nodes` = L 설정의 `ha_node_list`에서 L(과 새 노드)을 뺀 노드 + `ha_replica_list`에서 L을 뺀 노드. 케이스 3·4는 M이 L이라 빠지고, 케이스 5·6은 M이 들어감(M이 원격이므로).
- 확인 키(`confirm_master_restart`, `confirm_other_db_restart`)는 `run`에만 붙임. `plan`은 확인 키 없이 부르고, 결과의 `confirm_required`에 `run`에 붙여야 할 키가 나옴.

### 3.2 CMS → 헬퍼, CMS → 스크립트

| 받는 쪽 | 무엇을 | 어떻게 |
|---|---|---|
| 헬퍼 | 노드마다 호스트·포트·계정·지문·비밀번호 | fd 3 파이프로 한 줄씩(`호스트<TAB>포트<TAB>계정<TAB>지문<TAB>비밀번호`) 쓴 뒤 닫음 |
| 스크립트 | 비밀번호를 뺀 나머지 전부 | 명령줄 인자. CMS가 셸 없이 execv로 부르므로 값이 셸에서 해석되지 않음 |

스크립트 인자

```
ha_add_nodes.sh <plan|run|cleanup|rollback> --job-dir <작업 폴더> --agent <헬퍼 소켓>
  [--master-host <이름>]
  [--db <DB>]...                                 # 싱글일 때 HA로 넣을 DB, 여러 번
  [--confirm-master-restart] [--confirm-other-db-restart]
  --new <slave|replica>:<호스트>:<포트>:<계정>     # 새 노드, 여러 번
  [--existing <호스트>:<포트>:<계정>]...           # 기존 노드, 여러 번
  [--repl-log-home <경로>] [--backup-path <경로>] [--remote-backup-path <경로>]
  [--rollback-convert] [--no-start-ha]
```

- 지문은 스크립트 인자에 넣지 않음. 지문은 헬퍼가 들고 있고 헬퍼가 확인함(스크립트가 다른 지문으로 접속시킬 수 없음).
- 이름은 영문·숫자·`._-`, 경로는 절대 경로이고 공백·따옴표·`..`가 없어야 함. CMS와 스크립트가 둘 다 검사함. 원격 명령에 넣는 값은 모두 따옴표 처리함.
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
| 끝 | 스크립트가 끝날 때 `cm_ssh quit`. 스크립트가 죽으면 헬퍼는 스크립트 pid가 사라진 것을 보고 스스로 끝남(최대 대기 시간도 둠) |
| 터미널 실행 | 스크립트가 헬퍼를 `--ask`로 띄움. 헬퍼가 터미널에서 노드마다 `read`처럼 에코 없이 물어봄. 스크립트는 여전히 비밀번호를 모름 |

- 남는 위험: 작업이 도는 동안 같은 계정의 다른 프로세스가 소켓을 통해 헬퍼에 원격 명령을 시킬 수 있음. 같은 계정은 이미 CMS 계정 권한을 다 갖고 있으므로 새로 생기는 위험은 아님. 작업이 끝나면 소켓도 사라짐.

## 5. ssh 헬퍼 `cm_ssh` — 라이브러리 일원화

- expect·sshpass·시스템 ssh/scp 같은 외부 도구는 쓰지 않음. 원격 명령과 파일 전송을 모두 이 헬퍼 하나로 함.
- 라이브러리: CUBRID 빌드에 이미 들어 있는 OpenSSL 3.5.7(엔진 3rdparty, 정적 라이브러리) 위에 libssh2 1.11.1 하나만 더해 정적으로 링크함. CMS develop의 in-tree 빌드는 이미 엔진의 OpenSSL을 그대로 씀. 헬퍼는 CUBRID 라이브러리(libcubridcs 등)를 링크하지 않음.
- 코드: 이전 POC의 libssh2 코드(접속, 호스트 키 지문, 비밀번호·keyboard-interactive 인증, 원격 명령, SFTP 이어 올리기, 원격 sha256 확인, 데몬 출력 처리)를 옮겨 씀.
- 하위 명령

| 하위 명령 | 하는 일 |
|---|---|
| `agent --socket S --credentials-fd N` / `--ask` | 접속 정보를 받고 소켓에서 요청을 기다림. 준비되면 표준 출력에 `READY` 한 줄 |
| `hostkey <호스트> <포트>` | 지문만 읽음(인증 없음). `ha_check_ssh`의 첫 호출 |
| `check --agent S <호스트>` | 로그인, `uname -n`, CUBRID 경로·버전 |
| `exec --agent S <호스트> [--daemon] -- <명령>` | 원격 `bash -l -c`로 실행. 표준 입력을 원격으로 넘김. 출력은 그대로, 종료 코드도 그대로. `--daemon`은 출력을 원격 임시 파일로 받은 뒤 읽음 |
| `put --agent S <호스트> <로컬> <원격>` | SFTP로 올림. 끊기면 올린 자리부터. 끝나면 원격 sha256과 크기 비교 |
| `quit --agent S` | 헬퍼 종료 |

- 원격 노드에는 아무것도 설치하지 않음. 원격에서 할 일(설정 편집, 상태 읽기)은 스크립트의 "노드 동작" 부분을 `exec ... -- bash -s -- node <동작> <인자>`에 스크립트 자신을 표준 입력으로 보내 실행함. 로컬은 같은 함수를 헬퍼 없이 바로 부름. 그래서 로컬·원격이 같은 코드로 같은 일을 함(케이스 5에서 M이 원격이 되어도 코드가 같음).
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
| C2 | `cms_jobdir` | CMS | 잠금 확인(돌고 있는 작업이 있으면 거절), uuid, 작업 폴더(700) | C1 | 작업 폴더 |
| C3 | `cms_agent` | CMS → H | 헬퍼를 setsid로 띄우고 fd 3에 노드별 정보를 쓰고 닫음. `READY`를 최대 10초 기다림 | C2 | 소켓 준비. 실패하면 작업 폴더 정리 후 failure |
| C4 | `cms_spawn` | CMS → SC | 스크립트를 setsid로 execv(인자만, 비밀번호 없음), pid 기록 | C3 | 응답 `uuid`, `job_status`=`running` |
| C5 | `cms_status` | CMS | `ha_add_nodes_status`: 작업 폴더의 `job`·`steps`·`plan`·`log`를 읽고 pid가 살아 있는지 확인 | C4 | 진행 상태 |
| C6 | `cms_cancel` | CMS | `ha_add_nodes_cancel`: 스크립트 프로세스 그룹에 SIGTERM(한 번만) | C4 | 스크립트가 되돌리기로 들어감 |

### 6.2 스크립트 단계 (`run`)

단위: 1 = 한 번, 노드 = 노드마다, DB×노드 = DB마다 그리고 새 노드마다. 단계 이름 뒤 `[ ]`는 상태 응답의 `node`·`db`임.

| 묶음 | no | id | 단위 | 주체·대상 | 하는 일(명령) | 앞 조건 | 되돌리기 |
|---|---|---|---|---|---|---|---|
| A 확인 | S1 | `precheck` | 1 | SC @L | 인자 검사, L의 상태(싱글/마스터/슬레이브), `ha_db_list`, 두 목록과 기존 노드 입력 대조, 백업 자리(`df`) | C4 | 없음 |
| | S2 | `node_check [노드]` | 노드(E, X, Y) | SC → H @노드 | 로그인, `uname -n`, CUBRID 경로·버전, 모든 노드 이름이 풀리는지(`getent hosts`). E: D의 상태(M active, S·R standby), 그 노드 설정의 목록. X·Y: D가 꺼져 있는지, 다른 DB 서버·cub_master 상태 | S1, 헬퍼 준비 | 없음 |
| B 전환(1·2만) | S3 | `convert_stop` | 1 | SC @L | 떠 있는 DB 전부 `cubrid server stop` | S2 | 처음에 떠 있던 DB를 싱글로 기동 |
| | S4 | `convert_config` | 1 | SC @L | cubrid.conf `ha_mode=on`·`[@db] ha_mode=off`, cubrid_ha.conf(노드 1개), databases.txt. `.bak.<uuid>` | S3 | `.bak` 복원 |
| | S5 | `convert_restart` | 1 | SC @L | `cub_commdb -A` → `cubrid heartbeat start` | S4 | heartbeat 멈춤, cub_master 내림 |
| | S6 | `convert_verify` | 1 | SC @L | D 모두 active(최대 180초) | S5 | 없음 |
| | S7 | `convert_start_others` | 1 | SC @L | 고르지 않았던 DB를 싱글로 다시 기동 | S6 | 그 DB 멈춤 |
| C 준비 | S8 | `new_config [X/Y]` | 노드(X, Y) | SC → H @X/Y | cubrid.conf `ha_mode`, cubrid_ha.conf(최종 목록 + M의 [common] 나머지), databases.txt(D 줄). `.bak.<uuid>` | S2(전환이면 S7) | `.bak` 복원, 없던 파일은 지움 |
| | S9 | `backup [D]` | DB | SC @백업 원본 | `cubrid backupdb -C --no-check <D>@localhost`(DB마다 한 번만) | S8 전부 | 백업 폴더 지움 |
| | S10 | `send_backup [X/Y, D]` | DB×노드 | SC → H(SFTP) | 백업 파일 + bkvinf 올림, sha256 확인 | S9(그 D) | 받은 폴더 지움 |
| | S11 | `keep_old_db [X/Y, D]` | DB×노드 | SC → H @X/Y | 기존 볼륨·로그·복제 로그 폴더를 `.ha_add_nodes_<uuid>`로 옮겨 보관 | S10 | 제자리로 |
| | S12 | `restore [X/Y, D]` | DB×노드 | SC → H @X/Y | `cubrid restoreslave -s <원본 상태> -m <M> -B <받은 폴더> <D>` | S11 | 새 볼륨·로그 지움 |
| | S13 | `first_copylog [X/Y, D]` | DB×노드 | SC → H @X/Y | `cub_admin copylogdb -L <로그 폴더>/<D>_<M> -m async --start-page-id=-1 <D>@<M>` | S12 | 복제 로그 폴더 지움 |
| D 시작 | S14 | `new_ha_start [X]` | 노드(X1..Xa) | SC → H @X | HA 아닌 cub_master면 `cub_commdb -A` → `cubrid heartbeat start`(데몬 출력 처리) | 그 노드의 S13 전부 | `heartbeat stop`, cub_master를 작업 전 상태로 |
| | S15 | `new_ha_start [Y]` | 노드(Y1..Yb) | SC → H @Y | 같음 | S14 전부 | 같음 |
| | S16 | `peer_repl_start [X]` | 노드(a ≥ 2일 때 X마다) | SC → H @X | 다른 Xj의 Copylogdb가 없으면 `heartbeat repl start <Xj>` | S15 | `repl stop <Xj>`(띄운 것만) |
| E 등록 | S17 | `existing_config [E]` | 노드(M → S1..SN → R1..RM) | SC(@L) 또는 SC → H | 2.2 표대로 cubrid_ha.conf·databases.txt, `.bak.<uuid>`, `cubrid heartbeat reload` | S16 | `.bak` 복원 + reload |
| | S18 | `existing_repl_start [E]` | 노드(a ≥ 1일 때, 같은 순서) | SC(@L) 또는 SC → H | X마다 `cubrid heartbeat repl start <X>` | S17 전부 | X마다 `repl stop <X>`(그 노드의 S17 되돌리기보다 먼저), 새로 생긴 로그 폴더 지움 |
| F 확인 | S19 | `verify [X/Y]` | 노드(X, Y) | SC → H | D 모두 standby. X·Y에 `ha_node_list` 다른 노드 전부의 Copylogdb. E에 X마다 Copylogdb(최대 120초) | S18 | 없음 |
| | S20 | `cleanup` | 1 | SC, H | L과 X·Y의 이번 작업 백업 폴더 지움(옮겨 보관한 옛 DB는 남김), `cm_ssh quit` | S19 | 없음 |

- C 묶음은 DB 하나씩 끝냄: D1을 백업한 뒤 새 노드마다 S10~S13, 그다음 D2. 백업은 DB마다 한 번만 받고, 전송은 새 노드 수만큼 함.
- 단계 수 = 1 + (|E| + a + b) + [전환 5] + (a + b) + k + 4k(a + b) + a + b + [a ≥ 2이면 a] + |E| + [a ≥ 1이면 |E|] + (a + b) + 1. 예: 1:2:1에 X 2대, D 2개면 |E| = 4, a = 2, b = 0, k = 2 → 1 + 6 + 2 + 2 + 16 + 2 + 2 + 4 + 4 + 2 + 1 = 42단계. 정확한 표는 `plan`이 돌려주고, 화면은 그 표를 씀.

### 6.3 `plan`, `cleanup`, `rollback`

| 동작 | 단계 |
|---|---|
| `plan` | C1~C4 → S1, S2 → 결과(`plan` 파일: 노드 역할, 바뀔 목록 미리 보기, 재시작될 DB, 전송량, 6.2의 단계 표) → 헬퍼 종료. 아무것도 바꾸지 않음 |
| `cleanup` | C1~C4(새 노드 비밀번호만) → 옮겨 보관한 옛 DB 폴더 지움 |
| `rollback` | C1~C4(작업에 관계된 노드 비밀번호) → `undo` 기록을 거꾸로 실행. 스크립트가 죽어 `interrupted`가 된 작업에 씀 |

## 7. CMS와 스크립트 사이의 의존성

CMS에 HA 단계를 두지 않는 구조로 의존성을 대부분 없앰. 남는 것과 처리 방법은 다음과 같음.

| no | 무엇이 끝나야 | 무엇을 할 수 있음 | 처리 | 대기 |
|---|---|---|---|---|
| P1 | C2 작업 폴더 | C3·C4 | CMS 안에서 차례로 함 | 없음 |
| P2 | C3 헬퍼 준비(비밀번호 받음, 소켓 열림) | S2(첫 원격 접속) | CMS가 `READY`를 받은 뒤에만 C4를 함. 스크립트도 S2 앞에서 소켓을 최대 10초 확인함 | CMS가 최대 10초 |
| P3 | 스크립트의 상태 기록 | C5 상태 응답 | 스크립트가 `job`·`steps`를 임시 파일에 쓰고 mv로 바꿔 넣음. CMS는 그때그때 읽기만 함 | 없음 |
| P4 | C6 취소 신호 | 되돌리기 시작 | 스크립트가 TERM을 받으면 표시만 하고, 지금 명령이 끝나면 되돌리기로 감. `job`에 `phase=rollback`을 씀 | 없음(화면은 상태로 봄) |
| P5 | 스크립트 종료 | 헬퍼 종료 | 스크립트가 `cm_ssh quit`. 죽었으면 헬퍼가 스크립트 pid를 보고 스스로 끝남 | 없음 |
| P6 | S5(L의 cub_master 재기동) | L의 CMS 계속 동작 | `cub_commdb -A`는 CMS를 내리지 않음(이전 POC 실측). CMS의 DB 감시·조회가 잠깐 실패할 수 있음 | 없음 |
| P7 | CMS 재시작 | 작업 계속 | 스크립트·헬퍼가 setsid라 영향 없음. 새 CMS는 작업 폴더로 상태를 읽음 | 없음 |
| P8 | 작업이 도는 동안 | 같은 노드의 다른 CMS 작업(DB 시작·중지, HA 시작·중지, 백업 등) | 잠금 파일을 CMS의 HA·DB 변경 task들이 확인하고 거절하는 것을 권장함(이번 POC 범위 밖, 화면에서 막음) | — |
| P9 | `rollback`·`cleanup` | 비밀번호 | 비밀번호는 작업 폴더에 없으므로 C3을 다시 함(화면이 다시 입력받음) | — |

- 검토했으나 쓰지 않은 구조: CMS가 스크립트를 단계별로 나눠 차례로 부르는 방식(각 단계가 끝나면 CMS가 다음 단계를 부름). 중간에 기다림이 없어지는 대신 CMS가 흐름을 쥐게 됨. 그러면 CMS가 재시작될 때 작업이 멈추고, 되돌리기 순서도 CMS가 알아야 함. 그래서 스크립트 한 번 실행으로 끝까지 가고 CMS는 읽기만 하는 구조를 고름.

## 8. 케이스별 흐름 (1:N:M)

모든 케이스의 공통 순서: C1 → C2 → C3 → C4 → S1 → S2 → [S3~S7] → S8 → (D마다: S9 → 새 노드마다 S10~S13) → S14 → S15 → [S16] → S17 → [S18] → S19 → S20. C5는 화면이 계속 부르고, C6은 사용자가 취소할 때 부름.

| 케이스 | L | 백업 원본(S9) | S2 대상 | B 전환 | S14/S15 | S16 | S17·S18 대상 | `restoreslave -s` |
|---|---|---|---|---|---|---|---|---|
| 1 | 싱글 서버 | L | X1..Xa(+ 기존 노드 없음) | ○ | X만 | a ≥ 2 | L(M)만, S18은 X마다 | master |
| 2 | 싱글 서버 | L | Y1..Yb | ○ | Y만 | — | L(M)만, S18 없음 | master |
| 3 | M | L | S1..SN, R1..RM, X | — | X | a ≥ 2 | M, S1..SN, R1..RM | master |
| 4 | M | L | S1..SN, R1..RM, Y | — | Y | — | M, S1..SN, R1..RM(S18 없음) | master |
| 3+4 | M | L | E 전부, X, Y | — | X 다음 Y | a ≥ 2 | E 전부 | master |
| 5 | Sj | L(Sj) | M(원격), 다른 S, R, X, Y | — | X 다음 Y | a ≥ 2 | M(원격), L(로컬), 나머지 S, R | slave |
| 6 | Rj | L(Rj) | M(원격), S, 다른 R, X, Y | — | X 다음 Y | a ≥ 2 | M(원격), S, L(로컬), 나머지 R | replica |

- N = 0, M = 0: E = {M}. S2·S17·S18은 M 하나만 대상임.
- a = 0: S14·S16·S18이 없음. 기존 노드는 `ha_replica_list`만 바뀜.
- b = 0: S15가 없음. 기존 노드의 `ha_replica_list`는 그대로임.
- N, M이 클 때
  - S2·S17·S18은 노드 수만큼 차례로 돎. 실패한 노드에서 멈추고, 앞 노드들은 되돌림.
  - 노드 수가 늘면 노드마다 복제 프로세스가 늘어남(마스터·슬레이브는 (|ha_node_list| − 1) × k개씩 copylogdb·applylogdb, 레플리카는 |ha_node_list| × k개씩). `plan`에 노드별로 늘어나는 프로세스 수를 보여 줌.
  - DB 서버의 HA 예약 연결 수는 서버가 뜰 때 정해짐. 노드를 늘린 뒤 추가된 복제 연결은 일반 연결 자리(`max_clients`)를 씀. `plan`이 M의 `max_clients` 여유를 확인해 모자라면 알림.
  - 전송량은 DB 전체 백업 크기 × (a + b)임. 보관 로그가 첫 백업부터 마지막 새 노드의 HA 시작까지 남아 있어야 하므로, a + b가 크면 보관 로그 보존 설정을 미리 늘려야 함(`plan`이 전송량을 알려 줌).
  - 엔진 헤더에 heartbeat 노드 수 상수(8)가 있으나 11.4 소스에서 쓰는 곳을 찾지 못함. 큰 구성(노드 9대 이상)은 실측 항목으로 둠.
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
  - 일부 기존 노드만 X를 알고 있는 동안에도 heartbeat는 모르는 노드의 신호를 무시하므로 서비스에는 영향이 없음.
- 전환(케이스 1·2) 뒤 단계에서 실패하면 기본으로 전환 직후 상태(노드 1개 HA 마스터)까지만 되돌림. 전환까지 되돌리면 DB가 한 번 더 재시작되기 때문임. `--rollback-convert`면 싱글까지 되돌림.
- 취소: CMS가 스크립트의 프로세스 그룹에 SIGTERM을 보냄. 스크립트는 표시를 남기고, 지금 돌던 명령이 끝나면 되돌리기로 들어감. 되돌리기 중에는 TERM을 무시함(되돌리기 명령은 취소되지 않음).
- CMS 재시작: 영향 없음(7절 P7). 스크립트가 죽어 있는데 상태가 running이면 `interrupted`로 보이고, `rollback`으로 되돌릴 수 있음.
- 한 번에 한 작업: 스크립트가 `$CUBRID/tmp/ha_add_nodes/lock`을 flock으로 잡음. CMS도 C2에서 확인함.

## 10. CMS API

모두 admin 전용. 요청·응답은 CMS JSON 그대로.

| task | 동기/비동기 | 요청 | 응답 |
|---|---|---|---|
| `ha_check_ssh` | 동기(최대 약 20초) | 이전과 같음(`host`, `ssh_port`, `os_user`, `os_password`, `hostkey_sha256`) | 이전과 같음. 내부는 `cm_ssh hostkey`, `cm_ssh check`(일회용 헬퍼) |
| `ha_add_nodes` | 비동기 | 3.1의 JSON. `cleanup`·`rollback`은 `uuid` + 필요한 노드의 접속 정보·비밀번호 | `uuid`, `job_status`=`running` |
| `ha_add_nodes_status` | 동기 | `uuid`, `offset` | `job_status`, `phase`, `step_no`, `step_count`, `steps[]`(`id`, `no`, `title`, `node`, `db`, `state`, `detail`), `nodes[]`(`host`, `role`, `new`), `plan`(plan일 때), `output`, `next_offset`, `error`, `message`, `downtime_sec`, `kept_db_paths` |
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
  "confirm_required": ["confirm_master_restart", "confirm_other_db_restart"],
  "changes": [{"host": "nodeS1", "file": "cubrid_ha.conf", "key": "ha_node_list", "before": "...", "after": "..."}, ...],
  "backup_bytes": 0, "transfer_bytes": 0,
  "process_increase": [{"host": "nodeS1", "copylogdb": 2, "applylogdb": 2}, ...],
  "warnings": ["..."],
  "steps": [{"no": 1, "id": "precheck", "title": "...", "node": "", "db": "", "actor": "script"}, ...]
}
```

- `ha_check_ssh`의 첫 호출(지문 받기)은 화면이 노드마다 부름. 사용자가 지문을 확인한 뒤 그 지문을 `hostkey_sha256`으로 넘김.
- CMS가 스크립트를 부르기 전에 확인하는 것: 스크립트·헬퍼가 정해진 경로의 일반 파일이고 그룹·다른 사용자가 쓸 수 없는지.
- 이전 POC의 `ha_convert_master`·`ha_make_slave*` task는 이 API로 바뀜.

## 11. 사전 조건과 거절

- 모든 노드의 CUBRID 버전이 같아야 함.
- 모든 노드에서 모든 노드 이름이 풀려야 함. HA 노드 이름은 각 서버의 `uname -n`과 같아야 함.
- 기존 노드 입력은 M 설정의 두 목록과 같아야 함(2.2).
- 새 노드: D가 떠 있으면 안 됨. 다른 DB 서버가 떠 있으면 cub_master를 내려야 해서 거절함.
- 백업 자리: 백업 원본에 D 전체 백업 크기, 새 노드마다 받는 폴더에 같은 크기가 있어야 함(`plan`이 `df`로 확인).
- 방화벽(`ha_port_id` UDP, `cubrid_port_id` TCP)은 확인하지 못함. 안 열려 있으면 S19에서 실패함.

## 12. 확인할 것(N노드 시험 환경에서 실측)

| 항목 | 왜 |
|---|---|
| a ≥ 2일 때 새 노드끼리의 copylogdb가 heartbeat start만으로 뜨고 붙는지 | S16 필요 여부 |
| 새 레플리카: 기존 노드 reload만으로 붙는지, 모든 슬레이브의 로그를 받는지, failover 뒤에도 데이터가 이어지는지 | 2.1 표 |
| `repl start <레플리카>`가 실제로 "not listed"로 거부되는지 | 소스 확인만 함 |
| N, M이 클 때(노드 9대 이상 포함) heartbeat·복제가 정상인지 | 노드 수 상한 |
| 케이스 5: 슬레이브 백업 + `restoreslave -s slave` + M에서 첫 로그 복사로 따라붙는지 | 매뉴얼 절차와 다름 |
| 케이스 6: 레플리카 백업 + `restoreslave -s replica` + M에서 첫 로그 복사로 따라붙는지 | 8절 케이스 6 제약 |
| DBA 비밀번호가 걸린 DB에서 backupdb·restoreslave·changemode가 비밀번호 없이 되는지 | 고객 DB는 대부분 DBA 비밀번호가 있음 |
| 노드를 늘린 뒤 `max_clients`가 거의 찬 M에서 새 노드의 로그 복사가 붙는지 | HA 예약 연결 수 |
| 취소·실패 되돌리기 뒤 모든 노드가 시작 전과 같은지(설정 파일, DB 폴더, HA 프로세스, cub_master) — 실패 지점을 S2·S10·S14·S17(k번째 노드)·S18로 바꿔 가며 | 9절 |
| CMS 재시작 중에도 작업이 끝까지 가는지, 상태 조회가 맞는지 | 7절 P7 |
| 비밀번호가 `ps`, `/proc/*/cmdline`·`environ`, 작업 폴더, 로그, 원격 노드 어디에도 없는지, 헬퍼 메모리를 같은 계정이 못 읽는지 | 4절 |

## 13. 한계

- 여러 서버의 변경을 한 번에 성공시키거나 한 번에 취소하는 것은 안 됨. 되돌리기는 기록을 거꾸로 실행하는 최선의 노력임.
- 전환과 cub_master 재기동은 그 노드의 떠 있는 DB를 모두 재시작함(서비스 중단).
- 전송은 새 노드마다 차례로 함(병렬 전송·새 노드 사이 중계는 하지 않음).
- 같은 설정 파일을 다른 도구가 동시에 고치는 경우의 충돌 방지는 없음.
- 윈도우는 대상이 아님.

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
