================================================================================
 igh_pnp_step1.c  사용 안내 및 주의사항
 동적재구성SW Step 1 — IgH EtherCAT Master 기반 Plug-and-Play 타당성 확인
================================================================================

--------------------------------------------------------------------------------
0. 빠른 시작 (빌드 → 실행 → 결과 위치)
--------------------------------------------------------------------------------

[빌드]  (같은 디렉터리에서)
  $ gcc -O2 -Wall -o igh_pnp_step1 igh_pnp_step1.c \
        -I/usr/local/etherlab/include -L/usr/local/etherlab/lib -lethercat
  → 실행 파일 ./igh_pnp_step1 생성
  ※ IgH 설치 경로 확인: $ find / -name ecrt.h 2>/dev/null

[실행]  (같은 디렉터리에서, ethercat 서비스가 켜진 상태)
  $ sudo ./igh_pnp_step1 | tee run_log.txt
  $ sudo ./igh_pnp_step1 mirror | tee run_log.txt

[결과 위치]
  - 터미널(stdout)
      발견 정보, 상태 변화, 0.5초마다 DI/DO 모니터가 출력된다.
  - run_log.txt
      위처럼 tee를 붙였을 때, 터미널 출력이 그대로 저장된다.
  - discovered_slaves.json
      실행한 디렉터리(현재 작업 디렉터리)에 생성된다.
      sudo로 실행했으므로 파일 소유자가 root다.
      일반 사용자로 수정하려면:
        $ sudo chown $USER discovered_slaves.json
  - 에러 메시지(stderr)
      터미널에만 출력된다. 함께 저장하려면:
        $ sudo ./igh_pnp_step1 2>&1 | tee run_log.txt
  - 커널/master 로그
      $ dmesg | tail -50


[목차]
  0. 빠른 시작
  1. 목적
  2. 동작 흐름
  3. 사전 준비
  4. 빌드
  5. 실행
  6. 출력 결과 읽는 법
  7. 검증 방법
  8. 테스트 시나리오 (DIO 루프백 → L7 서보)
  9. 주의사항
 10. 알려진 한계
 11. 문제 해결
 12. 다음 단계(Step 2~3) 연결 포인트


--------------------------------------------------------------------------------
1. 목적
--------------------------------------------------------------------------------
- Vendor ID, Product Code, PDO Map을 코드에 하드코딩하지 않는다.
- 연결된 slave에서 이 정보를 Offline(PREOP)으로 읽어 온다.
- 읽어 온 정보만으로 slave 설정, PDO 매핑, Domain 등록을 자동으로 수행한다.
- Online(OP)에서 "입력 읽기 → 연산 → 출력 쓰기" 주기 루프가 도는지 확인한다.
=> 가장 기본적인 PnP 가능성 검증이 목적이다.


--------------------------------------------------------------------------------
2. 동작 흐름
--------------------------------------------------------------------------------
[Offline / PREOP]
  (1) ecrt_request_master(0)
        Master 요청
  (2) ecrt_master() 반복 호출
        버스 스캔(scan_busy)이 끝날 때까지 대기. 최대 10초.
  (3) ecrt_master_get_slave()
        Vendor ID, Product Code, Revision, Serial, Alias, 이름 획득
  (4) ecrt_master_get_sync_manager()
      ecrt_master_get_pdo()
      ecrt_master_get_pdo_entry()
        SM / PDO / Entry 맵 획득
  (5) ecrt_master_sdo_upload(0x1000:00)
        CoE를 지원하는 slave만 Device Type을 읽는다.
        하위 16비트 401 = I/O, 402 = 드라이브(CiA402)
  (6) discovered_slaves.json 저장
  (7) ecrt_master_create_domain()
        Domain 생성
  (8) ecrt_master_slave_config()
        Slave 설정 (발견한 VID/PC 사용)
  (9) ecrt_slave_config_pdos()
        발견한 PDO 맵을 그대로 적용
 (10) ecrt_slave_config_reg_pdo_entry()
        Domain에 Entry 등록. padding(index 0x0000)은 건너뛴다.

[Online / OP]
 (11) ecrt_master_activate()
 (12) 1 ms 주기 루프
        receive → process
        → 1-bit 입력(DI) 수집
        → 연산 (walk 모드 또는 mirror 모드)
        → 1-bit 출력(DO) 쓰기
        → queue → send
 (13) Ctrl+C 입력 시 루프 종료 → ecrt_release_master()

※ 원래 계획 순서와의 차이
  IgH에서는 Master 요청이 가장 먼저 온다.
  Domain 등록은 activate(OP 전환) 전에 반드시 끝나야 한다.


--------------------------------------------------------------------------------
3. 사전 준비
--------------------------------------------------------------------------------
- IgH EtherCAT Master (1.5.x 이상, 1.6 권장)가 설치되어 있고
  ethercat 서비스가 실행 중이어야 한다.
    $ sudo /etc/init.d/ethercat start      (또는 systemctl start ethercat)
    $ ethercat master                       → Link: UP 확인
    $ ethercat slaves                       → slave 목록 확인
- 사용자 공간 라이브러리 libethercat과 헤더 ecrt.h가 필요하다.
- 실시간 성능이 필요하면 PREEMPT_RT 커널을 권장한다.
  이 테스트 수준(1 ms, DIO)은 일반 커널에서도 동작한다.
- 다른 EtherCAT 애플리케이션(RAON-RT 등)이 master 0을 쓰고 있으면
  request_master가 실패한다. 먼저 종료할 것.


--------------------------------------------------------------------------------
4. 빌드
--------------------------------------------------------------------------------
  $ gcc -O2 -Wall -o igh_pnp_step1 igh_pnp_step1.c \
        -I/usr/local/etherlab/include -L/usr/local/etherlab/lib -lethercat

- include/lib 경로는 설치 위치(--prefix)에 맞게 수정한다.
  예: /opt/etherlab, /usr/local
- 실행할 때 libethercat.so를 못 찾으면:
    $ export LD_LIBRARY_PATH=/usr/local/etherlab/lib:$LD_LIBRARY_PATH
  또는 /etc/ld.so.conf.d/ 에 경로를 추가하고 ldconfig를 실행한다.


--------------------------------------------------------------------------------
5. 실행
--------------------------------------------------------------------------------
  $ sudo ./igh_pnp_step1            # walk 모드: DO를 0.5초마다 한 비트씩 순환 점등
  $ sudo ./igh_pnp_step1 mirror     # mirror 모드: DI 값을 DO로 그대로 복사

- sudo가 필요하다. mlockall과 SCHED_FIFO 우선순위 설정 때문이다.
  권한이 없으면 경고만 출력하고 계속 실행되지만, 주기 지터가 커진다.
- 종료는 Ctrl+C. master가 해제되면 slave는 PREOP로 돌아간다.
- 실행한 디렉터리에 discovered_slaves.json이 생성된다.


--------------------------------------------------------------------------------
6. 출력 결과 읽는 법
--------------------------------------------------------------------------------
[Offline 단계 출력 예시]
  [slave 1] EL1008 8K. Dig. Eingang 24V, 3ms
    vendor=0x00000002 product=0x03F03052 rev=0x00120000 serial=0 alias=0
    CoE=0 device_type=0x00000000 (no CoE (SII only))
    SM0 IN(TxPDO)
      PDO 0x1A00
        0x6000:01   1 bit
    ...

  - CoE=0     : 메일박스(CoE)가 없는 단순 단자. PDO 맵은 SII에서 읽은 고정값이다.
  - CiA402 drive : 서보 드라이브로 판별된 경우
  - (padding) : 정렬용 빈 entry. 등록하지 않는다.

[Online 단계 출력 예시]
  master: 3 slave(s), AL 0x08, link up
      → AL 0x08 = 모든 slave가 OP
  domain: WC 3, wc_state 2
      → wc_state 2 = COMPLETE (정상)
  DI:10000000  DO:10000000  S3:6041=592 ...

  - DI / DO   : 1-bit 입출력 비트열
  - S<n>:<index>=값 : 1비트가 아닌 입력 (예: 서보 Statusword 0x6041, 위치 0x6064)

  AL 상태 값
    0x01 = INIT
    0x02 = PREOP
    0x04 = SAFEOP
    0x08 = OP
    여러 값이 섞여 있으면 OR된 값이 나온다. (예: 0x0C = SAFEOP + OP 혼재)

  wc_state 값
    0 = ZERO
    1 = INCOMPLETE
    2 = COMPLETE


--------------------------------------------------------------------------------
7. 검증 방법
--------------------------------------------------------------------------------
(1) 발견한 정보가 master CLI 결과와 일치하는지 비교한다.
      $ ethercat slaves -v
      $ ethercat pdos -p <position>
    Vendor, Product, PDO/Entry 인덱스가 일치하면 "Offline 정보 확보" 성공이다.

(2) 프로그램 실행 중 다른 터미널에서 확인한다.
      $ ethercat slaves        → 모든 slave가 OP인지
      $ ethercat domains -v    → 도메인 데이터 값이 변하는지

(3) DIO 루프백(8장)에서 DO 패턴이 DI로 돌아오는지 확인한다.
    → 읽기 → 연산 → 쓰기 전체 사이클 검증

(4) 장치 구성을 바꿔서 코드 수정 없이 다시 실행한다.
    예: 단자 추가/제거, 순서 변경
    → 코드 수정 없이 동작하면 PnP 타당성이 확인된 것이다.


--------------------------------------------------------------------------------
8. 테스트 시나리오
--------------------------------------------------------------------------------
[A] 디지털 I/O 루프백 (1순위)
  - 구성 예: EK1100 커플러 + EL1008(DI 8ch) + EL2008(DO 8ch)
  - 배선: DO ch1 → DI ch1 (필요한 만큼 채널 추가), 공통 0V 연결
  - 실행: walk 모드 → DI 비트열이 DO 비트열을 따라가는지 확인
          (한 주기 지연될 수 있음)
  - 실행: mirror 모드 → DI에 스위치를 연결하고 누르면 대응 DO가 켜지는지 확인

[B] L7 서보 1축 (2순위)
  - 목적: CiA402 드라이브의 Identity, PDO 맵, Device Type(402)이 발견되는지 확인
  - 모니터 출력에서 Statusword(0x6041), 실제 위치(0x6064) 등이 읽히는지 확인
  - 이 코드는 Controlword를 0으로 유지하므로 모터는 Enable되지 않는다. (정상)

[C] L7 4축 확장 (3순위)
  - 4개 slave가 모두 동일한 VID/PC로 발견되고 각각 설정되는지 확인
  - 실제 구동(CSP)은 이 코드 범위 밖이다. 아래 주의사항 참고.


--------------------------------------------------------------------------------
9. 주의사항  ★ 반드시 읽을 것
--------------------------------------------------------------------------------
[안전]
  - 이 코드는 1-bit 출력(DO)에만 값을 쓴다.
    8/16/32-bit 출력(예: CiA402 Controlword 0x6040, 목표 위치 0x607A)은
    항상 0으로 유지된다. 따라서 서보가 연결되어 있어도 Enable되지 않는다.
    → 이 제한을 풀어서 서보를 구동하려면 별도 코드로 작성하고,
      비상정지(STO/E-Stop)를 준비한 뒤 진행할 것.
  - DO를 실제 액추에이터(솔레노이드, 릴레이, 브레이크 등)에 연결한 상태에서
    walk 모드를 돌리면 해당 장치가 순차적으로 동작한다.
    테스트 중에는 LED나 루프백 배선만 연결할 것.
  - 모터 브레이크 해제용 DO가 섞여 있는 구성에서는 절대 실행하지 말 것.

[배선]
  - DI 모듈의 입력 타입(p-switching / n-switching)과
    DO 모듈의 출력 타입을 매뉴얼에서 확인한 뒤 루프백 배선할 것.
  - 커플러의 전원 접점(Power contact) 24V / 0V가 공급되어야 DO 출력이 나온다.
    E-bus 전원과 현장 전원은 별개다.
  - DO 채널당 정격 전류를 초과하지 않도록 할 것.

[실시간성]
  - 주기 루프 안의 printf(0.5초마다)는 RT-safe하지 않다.
    타당성 확인용으로만 사용할 것.
    RAON-RT에 통합할 때는 non-RT 스레드나 링버퍼로 옮길 것.
  - 이 코드는 Distributed Clock(DC)을 설정하지 않는다.
    DIO는 문제없지만, 서보를 CSP/CSV 모드로 구동하려면
    ecrt_slave_config_dc()로 SYNC0 설정과 app time 동기화가 필요하다.
    (L7 드라이브 매뉴얼의 DC 요구사항 확인)

[PDO 맵]
  - 발견되는 PDO 맵은 "master가 스캔 시점에 알고 있는 현재 맵"이다.
    ・ CoE 드라이브: 드라이브에 저장된 현재 PDO 할당을 읽는다.
      이전에 다른 프로그램이 매핑을 바꿔 두었다면 그 값이 나온다.
    ・ 단순 단자: SII의 고정 맵이다.
  - 이 코드는 발견한 맵을 그대로 다시 적용한다.
    원하는 매핑(예: CSP용 Target Position 포함)으로 바꾸는 기능은 없다.
  - 1, 8, 16, 32 bit가 아닌 entry(예: 2-bit)는 값을 0으로 읽는다.
  - alias는 0, position 기반으로 설정한다.
    slave 순서(배선 순서)가 바뀌면 position도 바뀐다.

[기타]
  - 최대 slave 32개, slave당 PDO 32개, entry 128개,
    DI/DO 비트 각 256개까지 처리한다.
    초과하면 discovery가 실패한다. 필요하면 코드 상단 #define을 수정할 것.
  - SDO 0x1000 읽기는 activate 전에 blocking으로 수행한다.
    slave가 많으면 시작이 조금 느려질 수 있다.
  - JSON의 name 필드는 이스케이프 처리를 하지 않는다.
    이름에 따옴표가 있으면 JSON이 깨질 수 있다.


--------------------------------------------------------------------------------
10. 알려진 한계
--------------------------------------------------------------------------------
  - 장치 핫플러그(실행 중 추가/제거)는 지원하지 않는다.
    구성을 바꾸면 재실행해야 한다.
  - 연산은 데모 수준이다. (walk / mirror)
  - 서보 구동, 상태머신(CiA402 Enable 시퀀스), DC 동기화는 포함하지 않는다.
  - 모터/드라이버 상세 정보(0x6402, 0x6404, 0x6502 등)는 아직 읽지 않는다.
    → Step 2 범위


--------------------------------------------------------------------------------
11. 문제 해결
--------------------------------------------------------------------------------
  "request master failed"
    → ethercat 서비스 미실행, master 0이 없음, 또는 다른 프로그램이 master 사용 중.
      $ ethercat master 로 확인한다.

  "bus scan failed or link down"
    → 케이블, NIC 설정(/etc/ethercat.conf의 MASTER0_DEVICE),
      slave 전원을 확인한다.

  "PDO config failed" / "reg ... failed"
    → 발견한 맵에 비정상 entry가 있거나 상한을 초과한 경우.
      $ ethercat pdos 결과와 비교한다.

  activate는 됐는데 slave가 OP로 가지 않음 (AL 0x02 / 0x04에 머묾)
    → $ dmesg | tail 에서 AL status code를 확인한다.
      서보는 DC 미설정 때문에 SAFEOP에서 멈출 수 있다. (드라이브 설정에 따라 다름)

  wc_state가 INCOMPLETE(1)
    → 일부 slave가 프로세스 데이터 교환에 참여하지 못한 상태.
      해당 slave의 AL 상태와 에러를 확인한다.
        $ ethercat slaves -v
        $ ethercat sii_read -p <n>

  주기 지터가 큼
    → sudo로 실행했는지, PREEMPT_RT 커널인지, CPU 격리(isolcpus)를 했는지 확인한다.


--------------------------------------------------------------------------------
12. 다음 단계 연결 포인트
--------------------------------------------------------------------------------
[Step 2]
  - 0x1000 Device Type 하위 16비트로 드라이브(402)와 I/O(401)를 구분할 수 있다.
    CoE가 없는 장치는 VID/PC + ESI(XML)로 분류해야 한다.
  - 모터/드라이버 정적 정보는 PDO가 아니라 Object Dictionary에 있고 SDO로 읽는다.
      0x6402 모터 타입
      0x6404 모터 카탈로그 번호
      0x6502 지원 모드
      0x608F 엔코더 분해능 등
    CiA402에서 정의된 표준 인덱스지만, 실제로 값을 채우는지는 제조사마다 다르다.
    → L7에서 직접 확인이 필요하다.
  - discover_slave()에 SDO 업로드를 추가하는 방식으로 확장하면 된다.

[Step 3]
  - discovered_slaves.json을 RT-AIDE Configuration File 생성의 입력으로 사용한다.
  - JSON 구조: slaves[] → identity + syncs[] → pdos[] → entries[[index, subindex, bits]]

================================================================================
