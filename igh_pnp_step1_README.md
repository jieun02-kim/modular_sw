# igh_pnp_step1.c 사용 안내 및 주의사항

**동적재구성SW Step 1 — IgH EtherCAT Master 기반 Plug-and-Play 타당성 확인**

---

## 목차

0. [빠른 시작](#0-빠른-시작)
1. [목적](#1-목적)
2. [동작 흐름](#2-동작-흐름)
3. [사전 준비](#3-사전-준비)
4. [빌드](#4-빌드)
5. [실행](#5-실행)
6. [출력 결과 읽는 법](#6-출력-결과-읽는-법)
7. [검증 방법](#7-검증-방법)
8. [테스트 시나리오](#8-테스트-시나리오)
9. [주의사항](#9-주의사항)
10. [알려진 한계](#10-알려진-한계)
11. [문제 해결](#11-문제-해결)
12. [다음 단계 연결 포인트](#12-다음-단계-연결-포인트)

---

## 0. 빠른 시작

### 빌드 (같은 디렉터리에서)

IgH 설치 위치를 먼저 확인한다.

```bash
find / -name ecrt.h 2>/dev/null
find / -name "libethercat.so*" 2>/dev/null
```

기본 경로(`/usr/local/include`, `/usr/local/lib`)에 설치되어 있으면:

```bash
gcc -O2 -Wall -o igh_pnp_step1 igh_pnp_step1.c -lethercat
```

다른 경로(예: `/opt/etherlab`)에 설치되어 있으면 `-I`, `-L`로 지정한다:

```bash
gcc -O2 -Wall -o igh_pnp_step1 igh_pnp_step1.c \
    -I/opt/etherlab/include -L/opt/etherlab/lib -lethercat
```

→ 실행 파일 `./igh_pnp_step1` 생성

### 실행 (같은 디렉터리에서, ethercat 서비스가 켜진 상태)

```bash
sudo ./igh_pnp_step1 2>&1 | tee run_walk.txt          # walk 모드
sudo ./igh_pnp_step1 mirror 2>&1 | tee run_mirror.txt # mirror 모드
```

- 종료는 **Ctrl+C**
- 로그 파일 이름은 실행마다 다르게 둔다. 같은 이름이면 덮어쓴다.

### 결과 위치

| 결과 | 위치 / 내용 |
|---|---|
| 터미널(stdout) | 발견 정보, 상태 변화, 0.5초마다 DI/DO 모니터 |
| `run_*.txt` | `tee`로 저장된 터미널 출력 (`2>&1`을 붙이면 에러 메시지 포함) |
| `discovered_slaves.json` | 실행한 디렉터리에 생성. **실행할 때마다 덮어쓰므로** 필요하면 이름을 바꿔 보관 |
| 커널/master 로그 | `dmesg \| tail -50` |

> `discovered_slaves.json`은 sudo로 생성되어 소유자가 root다. 일반 사용자로 수정하려면:
> ```bash
> sudo chown $USER discovered_slaves.json
> ```

---

## 1. 목적

- Vendor ID, Product Code, PDO Map을 **코드에 하드코딩하지 않는다.**
- 연결된 slave에서 이 정보를 **Offline(PREOP)** 으로 읽어 온다.
- 읽어 온 정보만으로 slave 설정, PDO 매핑, Domain 등록을 자동으로 수행한다.
- **Online(OP)** 에서 "입력 읽기 → 연산 → 출력 쓰기" 주기 루프가 도는지 확인한다.

⇒ 가장 기본적인 PnP 가능성 검증이 목적이다.

---

## 2. 동작 흐름

### Offline / PREOP

| 단계 | API | 내용 |
|---|---|---|
| 1 | `ecrt_request_master(0)` | Master 요청 |
| 2 | `ecrt_master()` 반복 | 버스 스캔(`scan_busy`) 완료 대기, 최대 10초 |
| 3 | `ecrt_master_get_slave()` | Vendor ID, Product Code, Revision, Serial, Alias, 이름 획득 |
| 4 | `ecrt_master_get_sync_manager()`<br>`ecrt_master_get_pdo()`<br>`ecrt_master_get_pdo_entry()` | SM / PDO / Entry 맵 획득 |
| 5 | `ecrt_master_sdo_upload(0x1000:00)` | CoE 지원 slave만 Device Type 읽기. 하위 16비트 401 = I/O, 402 = 드라이브(CiA402) |
| 6 | — | `discovered_slaves.json` 저장 |
| 7 | `ecrt_master_create_domain()` | Domain 생성 |
| 8 | `ecrt_master_slave_config()` | Slave 설정 (발견한 VID/PC 사용) |
| 9 | `ecrt_slave_config_pdos()` | 발견한 PDO 맵을 그대로 적용 |
| 10 | `ecrt_slave_config_reg_pdo_entry()` | Domain에 Entry 등록. padding(index `0x0000`)은 건너뜀 |

### Online / OP

| 단계 | 내용 |
|---|---|
| 11 | `ecrt_master_activate()` |
| 12 | 1 ms 주기 루프: `receive → process → 1-bit 입력 수집 → 연산(walk/mirror) → 1-bit 출력 쓰기 → queue → send` |
| 13 | Ctrl+C 입력 시 루프 종료 → `ecrt_release_master()` |

> **원래 계획 순서와의 차이**
> IgH에서는 Master 요청이 가장 먼저 온다.
> Domain 등록은 activate(OP 전환) 전에 반드시 끝나야 한다.

---

## 3. 사전 준비

- IgH EtherCAT Master(1.5.x 이상, 1.6 권장)가 설치되어 있고 ethercat 서비스가 실행 중이어야 한다.
  ```bash
  sudo /etc/init.d/ethercat start   # 또는 systemctl start ethercat
  ethercat master                    # Link: UP 확인
  ethercat slaves                    # slave 목록 확인
  ```
- 사용자 공간 라이브러리 `libethercat`과 헤더 `ecrt.h`가 필요하다.
- 실시간 성능이 필요하면 PREEMPT_RT 커널을 권장한다. 이 테스트 수준(1 ms, DIO)은 일반 커널에서도 동작한다.
- 다른 EtherCAT 애플리케이션(RAON-RT 등)이 master 0을 쓰고 있으면 `request_master`가 실패한다. 먼저 종료할 것.

---

## 4. 빌드

```bash
gcc -O2 -Wall -o igh_pnp_step1 igh_pnp_step1.c -lethercat
```

- IgH가 기본 경로가 아닌 곳에 설치되어 있으면 `-I<설치경로>/include -L<설치경로>/lib`를 추가한다. (예: `/opt/etherlab`)
- 존재하지 않는 `-I`, `-L` 경로는 gcc가 무시하므로, 경로를 잘못 적어도 기본 경로에 설치되어 있으면 빌드는 된다.
- 실행할 때 `libethercat.so`를 못 찾으면:
  ```bash
  sudo ldconfig
  # 또는
  export LD_LIBRARY_PATH=<설치경로>/lib:$LD_LIBRARY_PATH
  ```

---

## 5. 실행

```bash
sudo ./igh_pnp_step1            # walk 모드: DO를 0.5초마다 한 비트씩 순환 점등
sudo ./igh_pnp_step1 mirror     # mirror 모드: DI 값을 DO로 그대로 복사
```

| 모드 | 동작 | 용도 |
|---|---|---|
| **walk** (기본) | 입력과 무관하게 DO를 순서대로 켬 | "쓰기" 확인 (LED 순환) |
| **mirror** | n번째 1-bit 입력 → n번째 DO 복사 | "읽기 → 연산 → 쓰기" 확인 |

- sudo가 필요하다. `mlockall`과 `SCHED_FIFO` 우선순위 설정 때문이다. 권한이 없으면 경고만 출력하고 계속 실행되지만 주기 지터가 커진다.
- 종료는 Ctrl+C. master가 해제되면 slave는 PREOP로 돌아간다.
- **mirror 모드 주의:** 코드는 모든 1-bit 입력을 DI로 취급한다. 아날로그 단자(예: EL3164)의 상태 비트도 DI 목록에 섞이므로, DI 단자가 없거나 순서가 뒤에 있으면 대응이 어긋난다.

---

## 6. 출력 결과 읽는 법

### Offline 단계 출력 예시

```
[slave 1] EL2004 4K. Dig. Ausgang 24V, 0.5A
  vendor=0x00000002 product=0x07D43052 rev=0x00100000 serial=0 alias=0
  CoE=0 device_type=0x00000000 (no CoE (SII only))
  SM0 OUT(RxPDO)
    PDO 0x1600
      0x7000:01   1 bit
  ...
```

| 항목 | 의미 |
|---|---|
| `[slave n]` | 네트워크상 순서(position). PC에 가까운 순서대로 0, 1, 2 ... |
| `vendor` | 제조사 ID (예: `0x00000002` = Beckhoff) |
| `product` | 제품 코드 (Beckhoff는 모델 번호 포함, 예: `0x07D4` = 2004) |
| `CoE=0` | 메일박스(CoE) 없음. PDO 맵은 SII에서 읽은 고정값 |
| `CoE=1` | SDO 지원 |
| `SM OUT(RxPDO)` | PC → 장치 방향 (출력) |
| `SM IN(TxPDO)` | 장치 → PC 방향 (입력) |
| `CiA402 drive` | 서보 드라이브로 판별된 경우 |
| `other profile` | CiA401/402가 아닌 프로파일 (예: Beckhoff MDP 5001) |
| `(padding)` | 정렬용 빈 entry. 등록하지 않음 |

### Online 단계 출력 예시

```
master: 3 slave(s), AL 0x08, link up
domain: WC 3, wc_state 2
DI:000000000000000000000000  DO:0100  S2:6000=0  S2:6000=0  S2:6000=2 ...
```

| 항목 | 의미 |
|---|---|
| `DI:` | 모든 1-bit 입력 비트열 (DI 단자 + 아날로그 단자 상태 비트 등) |
| `DO:` | 이번 주기에 쓴 1-bit 출력 비트열 |
| `S<n>:<index>=값` | 1비트보다 큰 입력. slave 번호, 오브젝트 인덱스, 값<br>(예: 서보 Statusword `0x6041`, 위치 `0x6064`) |

> 현재 모니터는 서브인덱스를 표시하지 않는다. 예를 들어 EL3164는 채널당 `Limit 1`, `Limit 2`, `측정값(:11)` 3개가 같은 인덱스로 찍히며, **세 번째가 측정값**이다.

**AL 상태 값**

| 값 | 상태 |
|---|---|
| `0x01` | INIT |
| `0x02` | PREOP |
| `0x04` | SAFEOP |
| `0x08` | OP |

여러 값이 섞여 있으면 OR된 값이 나온다. (예: `0x0C` = SAFEOP + OP 혼재)

**wc_state 값**

| 값 | 상태 |
|---|---|
| `0` | ZERO |
| `1` | INCOMPLETE (전환 중에 잠깐 나오는 것은 정상) |
| `2` | COMPLETE (정상) |

---

## 7. 검증 방법

1. **발견한 정보가 master CLI 결과와 일치하는지 비교한다.**
   ```bash
   ethercat slaves -v
   ethercat pdos -p <position>
   ```
   Vendor, Product, PDO/Entry 인덱스가 일치하면 "Offline 정보 확보" 성공이다.

2. **프로그램 실행 중 다른 터미널에서 확인한다.**
   ```bash
   ethercat slaves        # 모든 slave가 OP인지
   ethercat domains -v    # 도메인 데이터 값이 변하는지
   ```

3. **DIO 루프백(8장)에서 DO 패턴이 DI로 돌아오는지 확인한다.**
   → 읽기 → 연산 → 쓰기 전체 사이클 검증

4. **장치 구성을 바꿔서 코드 수정 없이 다시 실행한다.**
   예: 단자 추가/제거, 순서 변경
   → 코드 수정 없이 동작하면 PnP 타당성이 확인된 것이다.

---

## 8. 테스트 시나리오

### [A] 디지털 I/O (1순위)

- **구성 예:** EK1100 커플러 + EL1004(DI 4ch) + EL2004(DO 4ch) (+ EL2024, EL3164)
- **권장 배치:** `EK1100 → EL1004 → EL2004 → EL2024 → EL3164 (→ 엔드캡)`
  DI/DO가 앞쪽에 있으면 비트열이 읽기 쉽고 mirror 대응도 맞는다.
- **배선:** DO ch → DI ch 루프백. 0V는 전원 접점으로 공유되므로 전선 한 가닥이면 된다.
- **walk 모드:** DO LED 순환, DI 비트열이 DO를 따라가는지 확인 (한 주기 지연될 수 있음)
- **mirror 모드:** DI에 스위치를 연결하고 누르면 대응 DO가 켜지는지 확인

### [B] L7 서보 1축 (2순위)

- **목적:** CiA402 드라이브의 Identity, PDO 맵, Device Type(402)이 발견되는지 확인
- 모니터에서 Statusword(`0x6041`), 실제 위치(`0x6064`) 등이 읽히는지 확인
- 1-bit 출력이 없으므로 `0 digital output bit(s)`가 나오는 것이 정상
- 이 코드는 Controlword를 0으로 유지하므로 모터는 Enable되지 않는다. (정상)

### [C] L7 4축 확장 (3순위)

- 4개 slave가 모두 동일한 VID/PC로 발견되고 각각 설정되는지 확인
- 실제 구동(CSP)은 이 코드 범위 밖이다. 아래 주의사항 참고.

---

## 9. 주의사항

> ★ 반드시 읽을 것

### 안전

- 이 코드는 **1-bit 출력(DO)에만** 값을 쓴다. 8/16/32-bit 출력(예: CiA402 Controlword `0x6040`, 목표 위치 `0x607A`)은 항상 0으로 유지된다. 따라서 서보가 연결되어 있어도 Enable되지 않는다.
  → 이 제한을 풀어서 서보를 구동하려면 별도 코드로 작성하고, 비상정지(STO/E-Stop)를 준비한 뒤 진행할 것.
- DO를 실제 액추에이터(솔레노이드, 릴레이, 브레이크 등)에 연결한 상태에서 walk 모드를 돌리면 해당 장치가 순차적으로 동작한다. **테스트 중에는 LED나 루프백 배선만 연결할 것.**
- 모터 브레이크 해제용 DO가 섞여 있는 구성에서는 **절대 실행하지 말 것.**

### 배선 / 전원

- 커플러(EK1100)에 **Us(시스템 전원)** 와 **Up(전원 접점)** 두 곳 모두 24V DC를 넣어야 한다. 단자 개수와 관계없이 커플러에 한 번만 넣으면 옆 단자로 전달된다.
- 테스트용 전원은 24V DC 1~2A면 충분하다. 벤치 전원이면 전류 제한을 0.5~1A로 걸어 두면 단락 대비가 된다.
- 전압은 24V에 고정할 것. (EK1100 허용 범위 대략 20~29V)
- 단자 추가/제거와 배선은 **전원을 끈 상태에서** 할 것.
- EL3164 등 아날로그 입력(0~10V)에 **24V를 연결하지 말 것.**
- DI 모듈의 입력 타입(p-switching / n-switching)과 DO 모듈의 출력 타입을 매뉴얼에서 확인한 뒤 루프백 배선할 것.
- DO 채널당 정격 전류를 초과하지 않도록 할 것.

### 실시간성

- 주기 루프 안의 `printf`(0.5초마다)는 RT-safe하지 않다. 타당성 확인용으로만 사용할 것. RAON-RT에 통합할 때는 non-RT 스레드나 링버퍼로 옮길 것.
- 이 코드는 **Distributed Clock(DC)을 설정하지 않는다.** DIO는 문제없지만, 서보를 CSP/CSV 모드로 구동하려면 `ecrt_slave_config_dc()`로 SYNC0 설정과 app time 동기화가 필요하다. (L7 드라이브 매뉴얼의 DC 요구사항 확인)

### PDO 맵

- 발견되는 PDO 맵은 "master가 스캔 시점에 알고 있는 현재 맵"이다.
  - **CoE 드라이브:** 드라이브에 저장된 현재 PDO 할당을 읽는다. 이전에 다른 프로그램이 매핑을 바꿔 두었다면 그 값이 나온다.
  - **단순 단자:** SII의 고정 맵이다.
- 이 코드는 발견한 맵을 그대로 다시 적용한다. 원하는 매핑(예: CSP용 Target Position 포함)으로 바꾸는 기능은 없다.
- 1, 8, 16, 32 bit가 아닌 entry(예: 2-bit)는 값을 0으로 읽는다.
- alias는 0, position 기반으로 설정한다. slave 순서(배선 순서)가 바뀌면 position도 바뀐다.

### 기타

- 최대 slave 32개, slave당 PDO 32개, entry 128개, DI/DO 비트 각 256개까지 처리한다. 초과하면 discovery가 실패한다. 필요하면 코드 상단 `#define`을 수정할 것.
- SDO `0x1000` 읽기는 activate 전에 blocking으로 수행한다. slave가 많으면 시작이 조금 느려질 수 있다.
- JSON의 `name` 필드는 이스케이프 처리를 하지 않는다. 이름에 따옴표가 있으면 JSON이 깨질 수 있다.

---

## 10. 알려진 한계

- 장치 핫플러그(실행 중 추가/제거)는 지원하지 않는다. 구성을 바꾸면 재실행해야 한다.
- 연산은 데모 수준이다. (walk / mirror)
- 서보 구동, 상태머신(CiA402 Enable 시퀀스), DC 동기화는 포함하지 않는다.
- 모니터 출력에 서브인덱스와 항목 이름이 표시되지 않는다. (코드는 인덱스/서브인덱스/비트 크기만 알고 의미는 모른다)
- 모터/드라이버 상세 정보(`0x6402`, `0x6404`, `0x6502` 등)는 아직 읽지 않는다. → Step 2 범위

---

## 11. 문제 해결

| 증상 | 원인 / 조치 |
|---|---|
| `Failed to execute SDO upload: Protocol not supported` | **정상.** CoE가 없는 slave(EK1100, EL2004 등)에 `0x1000` 읽기를 시도해 거절된 것. IgH 라이브러리가 찍는 메시지이며, 코드는 이를 "CoE 없음"으로 처리한다. |
| `request master failed` | ethercat 서비스 미실행, master 0이 없음, 또는 다른 프로그램이 master 사용 중. `ethercat master`로 확인. |
| `bus scan failed or link down` | 케이블, NIC 설정(`/etc/ethercat.conf`의 `MASTER0_DEVICE`), slave 전원 확인. |
| `PDO config failed` / `reg ... failed` | 발견한 맵에 비정상 entry가 있거나 상한 초과. `ethercat pdos` 결과와 비교. |
| activate는 됐는데 OP로 가지 않음 (AL `0x02`/`0x04`에 머묾) | `dmesg \| tail`에서 AL status code 확인. 서보는 DC 미설정 때문에 SAFEOP에서 멈출 수 있다. (드라이브 설정에 따라 다름) |
| `wc_state`가 계속 INCOMPLETE(1) | 일부 slave가 데이터 교환에 참여하지 못함. `ethercat slaves -v`, `ethercat sii_read -p <n>`으로 확인. (전환 중 잠깐 1이 나오는 것은 정상) |
| `libethercat.so.1: cannot open shared object file` | `sudo ldconfig` 실행 후 재시도. |
| 주기 지터가 큼 | sudo로 실행했는지, PREEMPT_RT 커널인지, CPU 격리(`isolcpus`)를 했는지 확인. |

---

## 12. 다음 단계 연결 포인트

### Step 2

- `0x1000` Device Type 하위 16비트로 드라이브(402)와 I/O(401)를 구분할 수 있다.
  - 단, Beckhoff 단자처럼 자체 프로파일(MDP 5001)을 쓰는 장치도 있어 **프로파일 번호만으로는 분류가 부족하다.** (EL3164 실측: `0x012C1389`)
  - CoE가 없는 장치는 VID/PC + ESI(XML)로 분류해야 한다.
- 모터/드라이버 정적 정보는 **PDO가 아니라 Object Dictionary에 있고 SDO로 읽는다.**

  | 인덱스 | 내용 |
  |---|---|
  | `0x6402` | 모터 타입 |
  | `0x6404` | 모터 카탈로그 번호 |
  | `0x6502` | 지원 모드 |
  | `0x608F` | 엔코더 분해능 |

  CiA402에서 정의된 표준 인덱스지만, 실제로 값을 채우는지는 제조사마다 다르다. → L7에서 직접 확인 필요
- `discover_slave()`에 SDO 업로드를 추가하는 방식으로 확장하면 된다.

### Step 3

- `discovered_slaves.json`을 RT-AIDE Configuration File 생성의 입력으로 사용한다.
- JSON 구조:
  ```
  slaves[] → identity + syncs[] → pdos[] → entries[[index, subindex, bits]]
  ```
