# HA 노드 추가 — 스크립트 + 전용 CMS API 설계 (초안 v1)

싱글 DB를 HA로 바꾸고, HA에 슬레이브·레플리카를 여러 대 한 번에 추가하는 기능의 설계임. 동작은 셸 스크립트 하나에 넣고, CMS에는 그 스크립트만 부르는 얇은 전용 API를 둠. runscript는 쓰지 않음.

## 0. 범위와 전제

- 고객사 배포 환경을 가정함. 기존 HA에는 슬레이브 N대, 레플리카 N대가 있을 수 있음(0대 포함).
- 각 서버에 대해 주어지는 것은 OS 계정 아이디·비밀번호뿐임(ssh 키 배포 없음). 원격 작업은 ssh로 함.
- 한 번 실행으로 새 슬레이브 n대와 새 레플리카 n대를 같이 추가할 수 있음.
- 케이스

| 케이스 | 실행 위치 | 시작 상태 | 하는 일 |
|---|---|---|---|
| 1 | 마스터가 될 서버 | 싱글 DB | HA 마스터로 전환 + 새 슬레이브 추가 |
| 2 | 마스터가 될 서버 | 싱글 DB | HA 마스터로 전환 + 새 레플리카 추가 |
| 3 | 마스터 | HA | 새 슬레이브 추가 |
| 4 | 마스터 | HA | 새 레플리카 추가 |
| 5(선택) | 기존 슬레이브 | HA | 그 슬레이브를 백업 원본으로 써서 새 슬레이브·레플리카 추가. 마스터는 원격으로 다룸 |

- 1·2, 3·4는 같은 흐름임. 새 노드 목록에 슬레이브와 레플리카가 섞여 있어도 됨.
- 이전 POC(C++ task, 브랜치 poc/ha-make-slave)에서 실측으로 확인한 동작은 그대로 가져옴. 다른 점은 동작이 스크립트로 옮겨 가고, 새 노드를 여러 대 받고, 레플리카도 새로 추가한다는 점임.

## 1. 큰 그림

구성 요소는 셋임.

| 구성 요소 | 하는 일 | 하지 않는 일 |
|---|---|---|
| CMS 전용 API | 입력 형식 검사, 작업 폴더 만들기, 스크립트 실행, 비밀번호를 스크립트에 넘기기, 진행 상태·로그 읽어 주기, 취소 신호 보내기 | HA 판단, 설정 편집, 원격 명령 |
| 작업 스크립트 `ha_add_nodes.sh` | 확인, 전환, 설정 편집, 백업·전송·복원, HA 시작, 기존 노드 등록, 확인, 되돌리기, 진행 기록 | 비밀번호를 파일·인자·로그에 남기는 일 |
| ssh 헬퍼 `cm_ssh` | 원격 접속(호스트 키 고정, 비밀번호 인증), 원격 명령 실행, 파일 올리기(이어 올리기, sha256 확인) | 판단·흐름 |

흐름

1. 화면이 `ha_add_nodes`(action=`plan`)를 부름 → 스크립트가 모든 노드에 접속해 확인하고, 노드 역할·바뀔 목록·재시작될 DB·단계 표를 돌려줌. 아무것도 바꾸지 않음.
2. 화면이 결과를 보여 주고 확인을 받음.
3. 화면이 `ha_add_nodes`(action=`run`, 확인 키 포함)를 부름 → 스크립트가 뒤에서 돌고, 화면은 `ha_add_nodes_status`로 진행을 봄.
4. 실패·취소 시 스크립트가 바꾼 것을 거꾸로 되돌림.

- 스크립트는 CMS와 분리된 세션(setsid)으로 돎. CMS가 재시작돼도 스크립트는 계속 돌고, 재시작한 CMS는 작업 폴더의 파일로 상태를 읽음. 이전 POC의 "CMS 재시작 시 interrupted, 되돌리기 없음" 한계가 없어짐.
- 스크립트는 CMS 없이 터미널에서도 돌릴 수 있음(비밀번호는 프롬프트로 받음). DBA가 화면 없이 쓰거나 문제를 재현할 때 같은 코드를 씀.

## 2. 노드 역할별 설정 규칙 (매뉴얼·소스로 확인)

### 2.1 슬레이브와 레플리카의 차이

| 항목 | 새 슬레이브 | 새 레플리카 |
|---|---|---|
| cubrid.conf `ha_mode` | `on` | `replica` |
| `force_remove_log_archives` | `no`로 씀 | 엔진이 레플리카에서 `yes`로 고정함(설정하지 않음) |
| 넣을 목록 | 모든 노드의 `ha_node_list` | 모든 노드의 `ha_replica_list` |
| 새 노드 자신의 cubrid_ha.conf | `ha_node_list` = 마스터 + 기존 슬레이브 + 새 슬레이브 전부, `ha_replica_list` = 기존 + 새 레플리카 전부 | 같음. 자기 이름이 `ha_replica_list`에 있어야 heartbeat가 뜸 |
| `ha_copy_sync_mode` | 값 개수 = `ha_node_list` 노드 수. 새 슬레이브마다 모든 마스터·슬레이브 노드에서 값 하나 늘림 | 레플리카 노드에서는 값과 상관없이 항상 async. 레플리카는 `ha_node_list`에 없으므로 다른 노드의 값 개수도 그대로 |
| 로그를 받는 곳(새 노드 쪽 copylogdb) | `ha_node_list`의 다른 노드 전부 | `ha_node_list`의 노드 전부(다른 레플리카에서는 받지 않음) |
| 이 노드의 로그를 받는 곳 | 다른 슬레이브·마스터·모든 레플리카 | 없음 |
| 기존 노드에서 할 일 | 설정 + reload + `heartbeat repl start <새 슬레이브>` | 설정 + reload만. `repl start <레플리카>`는 엔진이 거부함(`ha_node_list`에 없는 이름) |
| restoreslave | `-s <백업 원본의 상태> -m <현재 마스터>` | 같음 |
| 첫 로그 복사 | 현재 마스터에서 `copylogdb --start-page-id=-1` | 같음 |

- 근거: 엔진이 copylogdb·applylogdb를 띄우는 대상은 `ha_node_list`의 노드뿐임. 매뉴얼의 "운영 중 레플리카 추가" 절차도 기존 노드는 `ha_replica_list`에 추가하고 reload만 하며, 레플리카용 복제 프로세스는 띄우지 말라고 함.
- reload가 다시 읽는 것은 노드 목록뿐이고 프로세스는 띄우지 않음. 그래서 새 슬레이브는 기존 노드마다 `repl start`가 필요함(이전 POC 실측: 이게 없으면 failover 직후 쓴 데이터가 옛 마스터에서 빠짐).

### 2.2 기존 노드에서 할 일

기존 노드 = 마스터 + 기존 슬레이브(`ha_node_list`) + 기존 레플리카(`ha_replica_list`). 입력에 하나라도 빠지면 시작 전에 거절함.

| 기존 노드 | cubrid_ha.conf | databases.txt | reload | repl start |
|---|---|---|---|---|
| 마스터·슬레이브 | `ha_node_list` += 새 슬레이브, `ha_copy_sync_mode` 값 늘림, `ha_replica_list` += 새 레플리카 | 대상 DB 줄의 호스트 += 새 슬레이브 | 함 | 새 슬레이브마다 |
| 레플리카 | 같음(`ha_copy_sync_mode`는 무시되지만 개수는 맞춰 둠) | 같음 | 함 | 새 슬레이브마다(레플리카도 슬레이브의 로그를 받음) |

### 2.3 새 노드끼리

- 새 노드의 설정에는 처음부터 최종 목록(새 노드 전부 포함)을 씀. 그래서 새 노드의 heartbeat가 뜰 때 새 노드끼리의 로그 복사도 같이 뜸.
- 확인할 것(실측): heartbeat start 때 아직 뜨지 않은 다른 새 슬레이브를 대상으로 한 copylogdb가 등록되고 그 노드가 뜨면 따라붙는지. 안 되면 모든 새 노드를 띄운 뒤 `peer_repl_start` 단계에서 빠진 것만 `repl start <다른 새 슬레이브>`로 띄움(단계는 설계에 넣어 두고, 빠진 것이 없으면 아무것도 하지 않음).
- 새 노드를 띄우는 순서는 새 슬레이브 먼저, 그다음 새 레플리카임.

### 2.4 ha_mode=on 노드의 싱글 DB (케이스 1·2)

- cubrid.conf `[@<db>]` 절에 `ha_mode=off`를 쓰면 HA 노드에서도 그 DB를 `cubrid server start`로 싱글로 띄울 수 있음(소스 확인, 11.4.5.1908 실측).
- 전환 때 HA로 넣지 않는 DB는 이 방법으로 싱글로 남김. cub_master를 다시 띄우면 그 노드의 DB 서버가 모두 내려가므로 떠 있던 DB는 전환 중에 멈췄다 다시 뜸(확인을 받음).

## 3. 케이스별 흐름

| 단계 묶음 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| A 확인(로컬, 모든 노드 접속) | ○ | ○ | ○ | ○ | ○ |
| B 전환(싱글 → 노드 1개 HA 마스터) | ○ | ○ | | | |
| C 새 노드 설정, DB마다 백업 1회 → 새 노드마다 전송·보관·복원·첫 로그 복사 | ○ | ○ | ○ | ○ | ○(백업 원본 = 실행한 슬레이브) |
| D 새 노드 HA 시작(슬레이브 먼저) | ○ | ○ | ○ | ○ | ○ |
| E 기존 노드 등록(설정·reload, 새 슬레이브면 repl start) | 마스터 | 마스터 | 기존 노드 전부 | 기존 노드 전부 | 기존 노드 전부(마스터는 원격) |
| F 확인·정리 | ○ | ○ | ○ | ○ | ○ |

- 전환 뒤 단계에서 실패하면 되돌리기는 기본으로 전환 직후 상태(노드 1개 HA 마스터)까지만 함. 전환까지 되돌리면 DB가 한 번 더 재시작되기 때문임. `rollback_convert=yes`면 싱글까지 되돌림.
- 케이스 5(미검증, 선택):
  - 실행한 슬레이브(L)에서 DB마다 온라인 백업을 받아 `restoreslave -s slave -m <마스터>`로 복원하고, 첫 로그 복사는 마스터에서 받음.
  - 매뉴얼의 슬레이브 원본 절차는 restoredb와 보관 로그 복사를 씀. restoreslave와 첫 로그 복사로 그 절차를 대신할 수 있는지는 실측해야 함.
  - 마스터 부하가 줄어드는 효과(백업 I/O)는 실측 뒤에 적음.

## 4. 단계 표

단계마다 `node`(대상 노드, 없으면 실행 노드)와 `db`(DB마다 하는 단계)가 붙음. 아래 L은 스크립트가 도는 노드(케이스 1~4는 마스터), M은 현재 마스터, X는 새 노드, E는 기존 노드임.

| 묶음 | id | 단위 | 어디서 | 하는 일 | 확인 | 되돌리기 |
|---|---|---|---|---|---|---|
| A | `precheck` | 1회 | L | 입력 검사, L의 상태(싱글/마스터/슬레이브), `ha_db_list`, 두 목록과 기존 노드 입력 대조, 백업 자리 | 실패하면 종료 | 없음 |
| A | `node_check` | 노드마다(E, X) | 각 노드 | ssh·지문·인증, `uname -n`, CUBRID 경로·버전, 모든 노드 이름이 풀리는지. E: 대상 DB 상태(M active, 슬레이브·레플리카 standby), 그 노드 설정의 목록. X: 대상 DB가 꺼져 있는지, 다른 DB 서버·cub_master 상태 | 위 조건 | 없음 |
| B | `convert_stop` | 1회 | L | 떠 있는 DB 전부 `cubrid server stop` | 명령 출력 | 처음에 떠 있던 DB를 싱글로 기동 |
| B | `convert_config` | 1회 | L | cubrid.conf `ha_mode=on`, 고르지 않은 DB `[@db] ha_mode=off`, cubrid_ha.conf(노드 1개), databases.txt | 다시 읽어 확인 | `.bak` 복원 |
| B | `convert_restart` | 1회 | L | `cub_commdb -A`, `cubrid heartbeat start` | 명령 출력 | heartbeat 멈춤, cub_master 내림 |
| B | `convert_verify` | 1회 | L | HA DB 모두 active | active | 없음 |
| B | `convert_start_others` | 1회 | L | 고르지 않았던 DB를 싱글로 다시 기동 | 명령 출력 | 그 DB 멈춤 |
| C | `new_config` | 노드마다(X) | X | cubrid.conf(`ha_mode`), cubrid_ha.conf(최종 목록, M의 [common] 나머지 값), databases.txt(대상 DB 줄) | 다시 읽어 확인 | `.bak` 복원(없던 파일은 지움) |
| C | `backup` | DB마다 | L | `cubrid backupdb -C --no-check <db>@localhost`(한 번만) | 종료 코드 | 백업 폴더 지움 |
| C | `send_backup` | DB×X | L→X | 백업 파일 + bkvinf 올림. 끊기면 이어 올림 | 크기·sha256 | X의 받은 폴더 지움 |
| C | `keep_old_db` | DB×X | X | 기존 볼륨·로그·복제 로그 폴더를 `.ha_add_nodes_<uuid>`로 옮겨 보관 | 옮겨졌는지 | 제자리로 |
| C | `restore` | DB×X | X | `restoreslave -s <원본 상태> -m <M>` | 종료 코드 | 새 볼륨·로그 지움 |
| C | `first_copylog` | DB×X | X | `copylogdb --start-page-id=-1 <db>@<M>` | 종료 코드 | 복제 로그 폴더 지움 |
| D | `new_ha_start` | 노드마다(새 슬레이브 → 새 레플리카) | X | HA 아닌 cub_master면 내린 뒤 `cubrid heartbeat start` | 명령 출력 | `heartbeat stop`, cub_master를 작업 전 상태로 |
| D | `peer_repl_start` | 노드마다(새 슬레이브가 2대 이상일 때) | X | 다른 새 슬레이브용 copylogdb가 없으면 `repl start <그 노드>` | heartbeat status | `repl stop <그 노드>` |
| E | `existing_config` | 노드마다(E) | E | 2.2 표대로 설정 + reload | 다시 읽어 확인, 명령 출력 | `.bak` 복원 + reload |
| E | `existing_repl_start` | 노드마다(E, 새 슬레이브가 있을 때) | E | 새 슬레이브마다 `repl start <X>` | 명령 출력 | `repl stop <X>`(설정 되돌리기보다 먼저), 새로 생긴 로그 폴더 지움 |
| F | `verify` | 노드마다(X) | X, E | X의 DB가 모두 standby. X에 `ha_node_list` 다른 노드 전부의 Copylogdb가 있음. E에 새 슬레이브용 Copylogdb가 있음 | 모두 있음 | 없음 |
| F | `cleanup` | 1회 | L, X | 이번 작업 백업 폴더 지움(옮겨 보관한 옛 DB는 남김) | 지워졌는지 | 없음 |

- 단계 수는 노드·DB 수에 따라 달라짐(예: 기존 노드 2대, 새 슬레이브 2대, DB 2개, HA에서 시작이면 36단계). 정확한 표는 `plan`이 돌려줌. 화면은 단계 표를 직접 만들지 않고 `plan`의 표를 씀.
- 백업은 DB마다 한 번만 받고 새 노드마다 보냄(새 노드 수만큼 백업하지 않음). 전송은 새 노드 수만큼 함.

## 5. 되돌리기·취소·재시작

- 되돌리기 기록: 바꾸는 명령을 하기 직전에 되돌리는 방법을 작업 폴더의 `undo` 파일에 한 줄씩 적음(노드, 종류, 경로 등. 비밀번호 없음). 실패·취소 때 이 파일을 거꾸로 실행함. 그래서 스크립트가 중간에 죽어도 나중에 같은 기록으로 되돌릴 수 있음(`rollback` 동작, 비밀번호 다시 필요).
- 순서 규칙(이전 POC 실측): `repl stop <노드>`는 설정에서 그 노드를 빼기 전에 함. 노드가 목록에서 빠진 뒤에는 실패하면서도 마지막 줄을 success로 찍으므로, 결과는 출력의 실패 문구로 판단함.
- 취소: CMS가 스크립트의 프로세스 그룹에 SIGTERM을 보냄. 스크립트는 취소 표시를 남기고, 지금 돌던 명령(헬퍼 포함)이 끝나면 되돌리기로 들어감. 되돌리기 중에는 TERM을 무시함(되돌리기 명령은 취소되지 않음).
- CMS 재시작: 스크립트는 setsid로 떠 있어 영향이 없음. 상태 조회는 작업 폴더의 파일과 pid(살아 있는지, 명령줄이 이 스크립트인지)로 판단함. 스크립트가 죽어 있는데 상태가 running이면 `interrupted`로 보이고, `rollback`으로 되돌릴 수 있음.
- 한 번에 한 작업: 스크립트가 `$CUBRID/tmp/ha_add_nodes/lock`을 flock으로 잡음. CMS도 시작 전에 확인함.

## 6. 비밀번호 경로

원칙: 비밀번호는 명령줄 인자, 환경 변수, 로그, 작업 폴더 파일, 원격 명령 문자열 어디에도 넣지 않음.

| 구간 | 방법 | 막는 것 |
|---|---|---|
| 화면 → CMS | 요청 JSON의 `os_password`(키에 `pass`가 있어 NCA 로그 가림 규칙에 걸림) | |
| CMS → 스크립트 | CMS가 파이프를 만들어 스크립트의 fd 3으로 넘기고, `호스트<TAB>비밀번호` 줄을 쓴 뒤 닫음. CMS는 메모리에서 지움 | 인자(`ps`), 환경 변수(`/proc/<pid>/environ`), 파일 |
| 스크립트 안 | fd 3을 읽어 연관 배열에만 둠(export 안 함). `set -x` 금지. 끝나면 unset | 로그 |
| 스크립트 → 헬퍼 | 호출할 때마다 `cm_ssh ... --password-fd 3 3< <(printf '%s\n' "$pw")`. printf는 셸 내장이라 프로세스 목록에 안 나옴. here-string·here-doc은 쓰지 않음(bash 5.1 전에는 임시 파일을 만듦) | 인자, 임시 파일 |
| 헬퍼 → 원격 | ssh 비밀번호 인증(password, keyboard-interactive)에만 씀. 원격 명령 문자열에는 넣지 않음. 읽은 뒤 메모리에서 지우고, core dump를 끔 | 원격 로그·프로세스 목록 |
| 터미널 실행 | fd 3이 없고 터미널이면 `read -rs`로 호스트마다 물어봄 | |

원격 ssh 방법 비교

| 방법 | 판단 | 이유 |
|---|---|---|
| 정적 링크 libssh2 헬퍼 `cm_ssh` | **채택** | 이전 POC 코드(호스트 키 고정, 비밀번호·keyboard-interactive 인증, 이어 올리기, sha256 확인)를 그대로 씀. OS에 ssh 라이브러리가 없어도 됨(배포판마다 libssh2/libssh 유무가 다름, 실측). OpenSSL도 정적으로 넣고 내보내기를 막음(CUBRID 라이브러리가 내보내는 OpenSSL과 충돌 방지, 이전 POC 실측) |
| 시스템 ssh + SSH_ASKPASS | 제외 | Rocky 8의 OpenSSH 8.0에는 `SSH_ASKPASS_REQUIRE`가 없어 DISPLAY·setsid 같은 우회가 필요함. askpass 프로그램이 비밀번호를 받을 곳이 결국 환경 변수나 파일임. 호스트 키 지문 고정도 임시 known_hosts가 필요함 |
| expect, sshpass | 제외 | 기본 설치가 아님 |
| ssh 키 | 제외 | 전제(아이디·비밀번호만 주어짐) |

## 7. 스크립트 구성

### 7.1 파일

| 파일 | 설치 위치(제안) | 비고 |
|---|---|---|
| `ha_add_nodes.sh` | `$CUBRID/share/cms/ha_add_nodes.sh` | CMS 빌드가 설치. 755. CMS는 이 경로만 부름 |
| `cm_ssh` | `$CUBRID/bin/cm_ssh` | CMS 빌드가 libssh2·OpenSSL을 소스에서 정적으로 빌드해 링크 |

- 원격 노드에서 하는 일(설정 편집, 상태 읽기 등)도 같은 스크립트의 "노드 동작" 부분으로 둠. 원격 실행은 `cm_ssh exec <노드> -- bash -l -s -- node <동작> <인자>`에 스크립트 자신을 표준 입력으로 보냄. 그래서 원격에는 아무것도 설치하지 않고, 로컬과 원격이 같은 코드로 같은 일을 함(로컬은 헬퍼 없이 바로 실행).
- 필요한 도구: bash 4.2 이상, awk, sed, sha256sum, getent, flock, setsid(모두 기본 설치). jq·python은 쓰지 않음.
- 데몬을 띄우는 명령(heartbeat start, cub_master)은 출력을 원격 임시 파일로 받은 뒤 읽음(데몬이 채널을 잡아 명령이 끝나지 않는 문제, 이전 POC 실측).

### 7.2 실행 형식

```
ha_add_nodes.sh plan|run|cleanup|rollback --job-dir <작업 폴더>
```

- 입력은 작업 폴더의 `input` 파일(비밀번호 없음). CMS가 요청 JSON을 검사한 뒤 씀. 터미널에서 돌릴 때는 사용자가 씀.

```
master_host=hamaster
db_names=hadb,hadb2            # 싱글일 때 HA로 넣을 DB(전환할 때만)
confirm_master_restart=yes
confirm_other_db_restart=yes
new=slave haslave 22 cubrid SHA256:...
new=replica harep2 22 cubrid SHA256:...
existing=hareplica 22 cubrid SHA256:...
existing=haslave1 22 cubrid SHA256:...
repl_log_home=                 # 비우면 각 노드 $CUBRID_DATABASES
backup_path=                   # 비우면 L의 $CUBRID_DATABASES/ha_add_nodes_backup
remote_backup_path=            # 비우면 각 새 노드의 $CUBRID_DATABASES/ha_add_nodes_backup
rollback_convert=no
start_ha=yes
```

- 이름·경로 값은 CMS와 스크립트가 둘 다 검사함(이름은 영문·숫자·`._-`, 경로는 절대 경로이고 공백·따옴표·`..` 없음). 원격 명령에 넣는 값은 모두 따옴표 처리함.
- 기존 노드의 역할(슬레이브/레플리카)은 입력으로 받지 않고 L의 설정에서 정함(케이스 5는 M의 설정을 원격으로 읽음).

### 7.3 작업 폴더

`$CUBRID/tmp/ha_add_nodes/<uuid>/`(L의 CMS 계정 소유, 700)

| 파일 | 내용 |
|---|---|
| `input` | 위 입력 |
| `pid` | 스크립트 pid |
| `job` | `status`, `phase`, `error`, `message`, `started_at`, `ended_at`, `downtime_sec`, `kept_db_paths` 등 한 줄씩 |
| `steps` | 단계마다 한 줄: 번호, id, 제목, node, db, state, detail(탭 구분) |
| `plan` | `plan` 결과: 노드 역할, 바뀔 목록, 재시작될 DB, 단계 표 |
| `undo` | 되돌리기 기록 |
| `log` | 실행 로그(명령과 출력) |

- `job`·`steps`는 임시 파일에 쓴 뒤 mv로 바꿔 넣음(읽는 쪽이 반쯤 쓴 파일을 보지 않게).

## 8. CMS API

모두 admin 전용. 요청·응답은 CMS JSON 그대로.

| task | 동기/비동기 | 요청 | 응답 |
|---|---|---|---|
| `ha_check_ssh` | 동기(최대 약 20초) | 이전과 같음 | 이전과 같음. 내부는 `cm_ssh check` 호출 |
| `ha_add_nodes` | 비동기 | `action`(`plan`/`run`/`cleanup`/`rollback`), 위 입력 값(JSON), 노드마다 `os_password`. `cleanup`·`rollback`은 `uuid` + 필요한 비밀번호 | `uuid`, `job_status`=`running` |
| `ha_add_nodes_status` | 동기 | `uuid`, `offset` | `job_status`, `phase`, `step_no`, `step_count`, `steps[]`(`id`, `no`, `title`, `node`, `db`, `state`, `detail`), `nodes[]`(`host`, `role`, `new`), `plan`(plan일 때), `output`, `next_offset`, `error`, `message`, `downtime_sec`, `kept_db_paths` |
| `ha_add_nodes_cancel` | 동기 | `uuid` | `job_status` |

- `plan`도 모든 노드에 접속하므로 비동기로 돌리고 같은 상태 API로 결과를 받음.
- CMS가 스크립트를 부르기 전에 확인하는 것: 스크립트가 정해진 경로의 일반 파일이고 그룹·다른 사용자가 쓸 수 없는지, 다른 작업이 돌고 있지 않은지.
- 이전 POC의 `ha_convert_master`·`ha_make_slave*` task는 이 API로 바뀜(전환은 `ha_add_nodes`의 `db_names`로 들어옴).

## 9. 사전 조건과 거절

- 모든 노드의 CUBRID 버전이 같아야 함.
- 모든 노드에서 모든 노드 이름이 풀려야 함(`getent hosts`). HA 노드 이름은 각 서버의 `uname -n`과 같아야 함.
- 기존 노드 입력 = L(또는 M) 설정의 `ha_node_list`(실행 노드 제외) + `ha_replica_list`. 빠지거나 넘치면 거절함.
- 새 노드: 대상 DB가 떠 있으면 안 됨. 다른 DB 서버가 떠 있으면 cub_master를 내려야 해서 거절함.
- 마스터의 보관 로그(`log_max_archives` 등)는 첫 백업부터 마지막 새 노드의 HA 시작까지 남아 있어야 함. 새 노드·DB가 많을수록 이 시간이 길어짐(사용자가 미리 맞춤. `plan`이 DB 크기와 새 노드 수로 대략의 전송량을 알려 줌).
- 백업 자리: L의 백업 폴더에 DB 전체 백업 크기, 새 노드마다 받는 폴더에 같은 크기가 있어야 함(`plan`이 `df`로 확인해 모자라면 거절).
- 방화벽(`ha_port_id` UDP, `cubrid_port_id` TCP)은 확인하지 못함. 안 열려 있으면 `verify`에서 실패함.

## 10. 확인할 것(시험 환경에서 실측)

| 항목 | 왜 |
|---|---|
| 새 슬레이브 2대 이상을 같이 추가할 때 새 노드끼리의 copylogdb가 heartbeat start만으로 뜨고 붙는지 | `peer_repl_start` 필요 여부 |
| 새 레플리카: 기존 노드 reload만으로 heartbeat에 붙는지, 레플리카가 모든 슬레이브의 로그를 받는지, failover 뒤에도 데이터가 이어지는지 | 2.1 표의 실제 동작 |
| `repl start <레플리카>`가 실제로 "not listed"로 거부되는지 | 소스 확인만 함 |
| 케이스 5: 슬레이브 백업 + `restoreslave -s slave` + 마스터에서 첫 로그 복사로 새 노드가 따라붙는지 | 매뉴얼 절차(restoredb + 보관 로그 복사)와 다름 |
| DBA 비밀번호가 걸린 DB에서 backupdb·restoreslave·changemode가 비밀번호 없이 되는지 | 고객 DB는 대부분 DBA 비밀번호가 있음 |
| 노드를 늘린 뒤 마스터 DB 서버의 HA 예약 연결 | 예약 수는 DB 서버가 뜰 때 정해짐. 추가된 복제 연결은 일반 연결 자리(`max_clients`)를 씀. `max_clients`가 꽉 차 있으면 새 노드의 로그 복사가 붙지 못함 |
| 취소·실패 되돌리기 뒤 모든 노드가 시작 전과 같은지(설정 파일, DB 폴더, HA 프로세스, cub_master) | 이전 POC와 같은 기준 |
| CMS 재시작 중에도 작업이 끝까지 가는지, 상태 조회가 맞는지 | 5절 |
| 비밀번호가 `ps`, `/proc/*/environ`, 작업 폴더, 로그, 원격 노드 어디에도 없는지 | 6절 |

## 11. 한계

- 여러 서버의 변경을 한 번에 성공시키거나 한 번에 취소하는 것은 안 됨. 되돌리기는 기록을 거꾸로 실행하는 최선의 노력임.
- 전환과 cub_master 재기동은 그 노드의 떠 있는 DB를 모두 재시작함(서비스 중단).
- 같은 설정 파일을 다른 도구가 동시에 고치는 경우의 충돌 방지는 없음.
- 윈도우는 대상이 아님.

## 부록. 근거

| 내용 | 근거 |
|---|---|
| copylogdb·applylogdb는 `ha_node_list` 노드만 대상, 레플리카 노드의 복사 모드는 항상 async | 엔진 `src/executables/util_common.c`(HA 설정 읽기), `util_service.c`(copylogdb 시작, 목록에 없는 노드면 "not listed") |
| 레플리카는 자기 이름이 `ha_replica_list`에 있어야 함, 두 목록의 그룹 이름이 같아야 함 | 엔진 `src/executables/master_heartbeat.c`(노드 목록 읽기) |
| `[@db]` 절 `ha_mode`가 DB별로 적용됨, off면 cubrid_ha.conf를 읽지 않음 | 엔진 `src/base/system_parameter.c`, `util_service.c`(server start/stop) + 실측 |
| HA 예약 연결 수는 서버가 뜰 때 정해지고, 모자라면 일반 연결을 씀 | 엔진 `src/connection/connection_globals.c`, `connection_sr.c` |
| 운영 중 슬레이브 추가: 기존 노드 설정 + reload + `repl start <새 노드>` / 레플리카 추가: `ha_replica_list` + reload, 복제 프로세스 띄우지 않음 / restoreslave `-s`·`-m` | CUBRID 11.4 매뉴얼 HA 장(운영 중 슬레이브 추가, 운영 중 레플리카 추가, restoreslave) |
| reload만으로는 새 슬레이브용 프로세스가 안 뜸, `repl stop` 순서, 데몬 출력 처리, 정적 OpenSSL 링크 순서 | 이전 POC 실측(브랜치 poc/ha-make-slave) |
