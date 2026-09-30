/*
 * igh_pnp_step1.c
 * 동적재구성SW Step 1 — IgH EtherCAT Master 기반 Plug-and-Play 타당성 확인
 *
 *  [Offline / PREOP 단계]  (코드에 Vendor ID, PDO 맵 하드코딩 없음)
 *   1. Master 요청 → 버스 스캔 완료 대기
 *   2. 각 slave Identity 획득 (Vendor ID, Product Code, Revision, Serial)
 *   3. 각 slave SM / PDO / PDO Entry 맵 획득 (master가 스캔 때 SII/CoE로 읽어둔 값)
 *   4. CoE 지원 slave면 0x1000 Device Type SDO 읽기 → 프로파일 분류 (401=I/O, 402=Drive)
 *   5. 발견 정보 JSON 덤프 (Step 3 cfg 생성 입력용)
 *   6. 발견한 맵 그대로 slave config → PDO config → domain 등록
 *  [Online / OP 단계]
 *   7. activate → 1 ms 주기: 입력 읽기 → 연산 → 출력 쓰기
 *
 * 안전: 1-bit 출력(DO)만 씀. 8/16/32-bit 출력(예: CiA402 Controlword)은 0 유지
 *       → 서보 드라이브가 물려 있어도 Enable 되지 않음.
 *
 * build: gcc -O2 -Wall -o igh_pnp_step1 igh_pnp_step1.c \
 *            -I/usr/local/etherlab/include -L/usr/local/etherlab/lib -lethercat
 * run  : sudo ./igh_pnp_step1          (DO walking 패턴)
 *        sudo ./igh_pnp_step1 mirror   (DI → DO 그대로 복사)
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include "ecrt.h"

#define PERIOD_NS     1000000L
#define NSEC_PER_SEC  1000000000L
#define MAX_SLAVES    32
#define MAX_SYNCS     EC_MAX_SYNC_MANAGERS
#define MAX_PDOS      32
#define MAX_ENTRIES   128
#define MAX_BITS      256

typedef struct {
    ec_slave_info_t     info;
    int                 has_coe;
    uint32_t            device_type;

    /* 발견한 맵을 IgH 구조체 그대로 보관 → ecrt_slave_config_pdos()에 바로 전달 */
    ec_sync_info_t      syncs[MAX_SYNCS];
    unsigned int        n_syncs;
    ec_pdo_info_t       pdos[MAX_PDOS];
    unsigned int        n_pdos;
    ec_pdo_entry_info_t entries[MAX_ENTRIES];
    ec_direction_t      entry_dir[MAX_ENTRIES];
    int                 entry_off[MAX_ENTRIES];   /* domain byte offset, -1 = 미등록 */
    unsigned int        entry_bit[MAX_ENTRIES];
    unsigned int        n_entries;

    ec_slave_config_t  *sc;
} slave_t;

static ec_master_t *master;
static ec_domain_t *domain;
static uint8_t     *pd;
static slave_t      slaves[MAX_SLAVES];
static unsigned int n_slaves;
static unsigned int n_do;
static volatile sig_atomic_t run = 1;

static void on_sigint(int s) { (void)s; run = 0; }

/* ------------------------------------------------------------------ */
/* Offline: 스캔 & 정보 획득                                           */
/* ------------------------------------------------------------------ */
static int wait_scan(ec_master_info_t *mi)
{
    for (int i = 0; i < 100; i++) {                 /* 최대 10 s */
        if (ecrt_master(master, mi)) return -1;
        if (!mi->scan_busy) return 0;
        usleep(100000);
    }
    return -1;
}

static int discover_slave(uint16_t pos, slave_t *sl)
{
    memset(sl, 0, sizeof(*sl));
    if (ecrt_master_get_slave(master, pos, &sl->info)) return -1;

    for (uint8_t sm = 0; sm < sl->info.sync_count && sm < EC_MAX_SYNC_MANAGERS; sm++) {
        ec_sync_info_t sync;
        if (ecrt_master_get_sync_manager(master, pos, sm, &sync)) return -1;
        if (sync.n_pdos == 0) continue;             /* mailbox SM 등 PDO 없는 SM 제외 */
        if (sl->n_syncs >= MAX_SYNCS) return -1;

        ec_sync_info_t *dst = &sl->syncs[sl->n_syncs++];
        *dst = sync;
        dst->pdos = &sl->pdos[sl->n_pdos];

        for (uint16_t p = 0; p < sync.n_pdos; p++) {
            if (sl->n_pdos >= MAX_PDOS) return -1;
            ec_pdo_info_t *pdo = &sl->pdos[sl->n_pdos++];
            if (ecrt_master_get_pdo(master, pos, sm, p, pdo)) return -1;
            pdo->entries = &sl->entries[sl->n_entries];

            for (uint16_t e = 0; e < pdo->n_entries; e++) {
                if (sl->n_entries >= MAX_ENTRIES) return -1;
                if (ecrt_master_get_pdo_entry(master, pos, sm, p, e,
                                              &sl->entries[sl->n_entries]))
                    return -1;
                sl->entry_dir[sl->n_entries] = sync.dir;
                sl->entry_off[sl->n_entries] = -1;
                sl->n_entries++;
            }
        }
    }

    /* CoE 지원 slave면 0x1000 Device Type (미지원이면 실패 → has_coe = 0) */
    uint8_t  buf[4];
    size_t   rs = 0;
    uint32_t abort_code = 0;
    if (ecrt_master_sdo_upload(master, pos, 0x1000, 0x00, buf, sizeof(buf),
                               &rs, &abort_code) == 0 && rs == 4) {
        sl->has_coe = 1;
        sl->device_type = EC_READ_U32(buf);
    }
    return 0;
}

static const char *profile_str(const slave_t *sl)
{
    if (!sl->has_coe) return "no CoE (SII only)";
    switch (sl->device_type & 0xFFFF) {
    case 401: return "CiA401 generic I/O";
    case 402: return "CiA402 drive";
    default:  return "other profile";
    }
}

static void print_slave(const slave_t *sl)
{
    const ec_slave_info_t *i = &sl->info;
    printf("\n[slave %u] %s\n", i->position, i->name);
    printf("  vendor=0x%08X product=0x%08X rev=0x%08X serial=%u alias=%u\n",
           i->vendor_id, i->product_code, i->revision_number,
           i->serial_number, i->alias);
    printf("  CoE=%d device_type=0x%08X (%s)\n",
           sl->has_coe, sl->device_type, profile_str(sl));

    for (unsigned y = 0; y < sl->n_syncs; y++) {
        const ec_sync_info_t *s = &sl->syncs[y];
        printf("  SM%u %s\n", s->index, s->dir == EC_DIR_OUTPUT ? "OUT(RxPDO)" : "IN(TxPDO)");
        for (unsigned p = 0; p < s->n_pdos; p++) {
            const ec_pdo_info_t *pdo = &s->pdos[p];
            printf("    PDO 0x%04X\n", pdo->index);
            for (unsigned e = 0; e < pdo->n_entries; e++) {
                const ec_pdo_entry_info_t *en = &pdo->entries[e];
                printf("      0x%04X:%02X  %2u bit%s\n", en->index, en->subindex,
                       en->bit_length, en->index == 0 ? "  (padding)" : "");
            }
        }
    }
}

static void dump_json(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    fprintf(f, "{\n  \"slaves\": [\n");
    for (unsigned s = 0; s < n_slaves; s++) {
        const slave_t *sl = &slaves[s];
        const ec_slave_info_t *i = &sl->info;
        fprintf(f, "    {\"position\": %u, \"name\": \"%s\", \"vendor_id\": \"0x%08X\", "
                   "\"product_code\": \"0x%08X\", \"revision\": \"0x%08X\", \"serial\": %u, "
                   "\"coe\": %s, \"device_type\": \"0x%08X\",\n     \"syncs\": [",
                i->position, i->name, i->vendor_id, i->product_code,
                i->revision_number, i->serial_number,
                sl->has_coe ? "true" : "false", sl->device_type);
        for (unsigned y = 0; y < sl->n_syncs; y++) {
            const ec_sync_info_t *sy = &sl->syncs[y];
            fprintf(f, "%s\n      {\"sm\": %u, \"dir\": \"%s\", \"pdos\": [", y ? "," : "",
                    sy->index, sy->dir == EC_DIR_OUTPUT ? "out" : "in");
            for (unsigned p = 0; p < sy->n_pdos; p++) {
                const ec_pdo_info_t *pdo = &sy->pdos[p];
                fprintf(f, "%s{\"index\": \"0x%04X\", \"entries\": [", p ? ", " : "", pdo->index);
                for (unsigned e = 0; e < pdo->n_entries; e++) {
                    const ec_pdo_entry_info_t *en = &pdo->entries[e];
                    fprintf(f, "%s[\"0x%04X\", %u, %u]", e ? ", " : "",
                            en->index, en->subindex, en->bit_length);
                }
                fprintf(f, "]}");
            }
            fprintf(f, "]}");
        }
        fprintf(f, "]}%s\n", s + 1 < n_slaves ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    printf("\n>> discovered info written to %s\n", path);
}

/* ------------------------------------------------------------------ */
/* Offline: 발견한 맵으로 설정 (Domain 생성 → Slave config → PDO → 등록) */
/* ------------------------------------------------------------------ */
static int configure(void)
{
    domain = ecrt_master_create_domain(master);
    if (!domain) return -1;

    for (unsigned s = 0; s < n_slaves; s++) {
        slave_t *sl = &slaves[s];
        sl->sc = ecrt_master_slave_config(master, 0, sl->info.position,
                                          sl->info.vendor_id, sl->info.product_code);
        if (!sl->sc) return -1;

        if (sl->n_syncs && ecrt_slave_config_pdos(sl->sc, sl->n_syncs, sl->syncs)) {
            fprintf(stderr, "slave %u: PDO config failed\n", s);
            return -1;
        }
        for (unsigned e = 0; e < sl->n_entries; e++) {
            const ec_pdo_entry_info_t *en = &sl->entries[e];
            if (en->index == 0x0000) continue;       /* padding(gap) entry */
            int off = ecrt_slave_config_reg_pdo_entry(sl->sc, en->index, en->subindex,
                                                      domain, &sl->entry_bit[e]);
            if (off < 0) {
                fprintf(stderr, "slave %u: reg 0x%04X:%02X failed\n", s, en->index, en->subindex);
                return -1;
            }
            sl->entry_off[e] = off;
            if (sl->entry_dir[e] == EC_DIR_OUTPUT && en->bit_length == 1 && n_do < MAX_BITS)
                n_do++;
        }
    }
    return 0;
}

static uint32_t read_entry(const slave_t *sl, unsigned e)
{
    const uint8_t *p = pd + sl->entry_off[e];
    switch (sl->entries[e].bit_length) {
    case 1:  return EC_READ_BIT(p, sl->entry_bit[e]);
    case 8:  return EC_READ_U8(p);
    case 16: return EC_READ_U16(p);
    case 32: return EC_READ_U32(p);
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Online: 주기 루프                                                    */
/* ------------------------------------------------------------------ */
static void cyclic(int mirror)
{
    struct timespec t;
    unsigned long cycle = 0;
    unsigned int walk = 0, n_di;
    uint8_t di[MAX_BITS], dout[MAX_BITS];
    ec_domain_state_t ds, ds_old = {0};
    ec_master_state_t ms, ms_old = {0};

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (run) {
        t.tv_nsec += PERIOD_NS;
        while (t.tv_nsec >= NSEC_PER_SEC) { t.tv_nsec -= NSEC_PER_SEC; t.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);

        ecrt_master_receive(master);
        ecrt_domain_process(domain);

        /* 상태 감시 */
        ecrt_domain_state(domain, &ds);
        if (ds.wc_state != ds_old.wc_state)
            printf("domain: WC %u, wc_state %u\n", ds.working_counter, ds.wc_state);
        ds_old = ds;
        ecrt_master_state(master, &ms);
        if (ms.al_states != ms_old.al_states || ms.slaves_responding != ms_old.slaves_responding)
            printf("master: %u slave(s), AL 0x%02X, link %s\n",
                   ms.slaves_responding, ms.al_states, ms.link_up ? "up" : "down");
        ms_old = ms;

        /* (1) slave 제어 정보 확보: 1-bit 입력 수집 */
        n_di = 0;
        for (unsigned s = 0; s < n_slaves; s++)
            for (unsigned e = 0; e < slaves[s].n_entries; e++)
                if (slaves[s].entry_off[e] >= 0 && slaves[s].entry_dir[e] == EC_DIR_INPUT &&
                    slaves[s].entries[e].bit_length == 1 && n_di < MAX_BITS)
                    di[n_di++] = (uint8_t)read_entry(&slaves[s], e);

        /* (2) 연산 */
        if (cycle % 500 == 0) walk++;
        for (unsigned k = 0; k < n_do; k++)
            dout[k] = mirror ? (k < n_di ? di[k] : 0) : (k == walk % n_do);

        /* (3) 연산 값 slave로 전송: 1-bit 출력만 */
        unsigned k = 0;
        for (unsigned s = 0; s < n_slaves; s++)
            for (unsigned e = 0; e < slaves[s].n_entries; e++)
                if (slaves[s].entry_off[e] >= 0 && slaves[s].entry_dir[e] == EC_DIR_OUTPUT &&
                    slaves[s].entries[e].bit_length == 1 && k < n_do)
                    EC_WRITE_BIT(pd + slaves[s].entry_off[e], slaves[s].entry_bit[e], dout[k++]);

        /* 모니터링 (타당성 확인용. RT 통합 시에는 non-RT 스레드로 이동) */
        if (cycle % 500 == 0) {
            printf("DI:");
            for (unsigned i = 0; i < n_di; i++) printf("%u", di[i]);
            printf("  DO:");
            for (unsigned i = 0; i < n_do; i++) printf("%u", dout[i]);
            for (unsigned s = 0; s < n_slaves; s++)
                for (unsigned e = 0; e < slaves[s].n_entries; e++)
                    if (slaves[s].entry_off[e] >= 0 && slaves[s].entry_dir[e] == EC_DIR_INPUT &&
                        slaves[s].entries[e].bit_length > 1)
                        printf("  S%u:%04X=%u", s, slaves[s].entries[e].index,
                               read_entry(&slaves[s], e));
            printf("\n");
        }

        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        cycle++;
    }
}

int main(int argc, char **argv)
{
    int mirror = (argc > 1 && strcmp(argv[1], "mirror") == 0);
    ec_master_info_t mi;

    signal(SIGINT, on_sigint);
    if (mlockall(MCL_CURRENT | MCL_FUTURE)) perror("mlockall");

    master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "request master failed\n"); return 1; }

    /* ===== Offline ===== */
    if (wait_scan(&mi) || !mi.link_up) {
        fprintf(stderr, "bus scan failed or link down\n");
        goto out;
    }
    n_slaves = mi.slave_count > MAX_SLAVES ? MAX_SLAVES : mi.slave_count;
    printf("link up, %u slave(s) found\n", mi.slave_count);

    for (unsigned s = 0; s < n_slaves; s++) {
        if (discover_slave(s, &slaves[s])) {
            fprintf(stderr, "slave %u: discovery failed\n", s);
            goto out;
        }
        print_slave(&slaves[s]);
    }
    dump_json("discovered_slaves.json");

    if (configure()) { fprintf(stderr, "configure failed\n"); goto out; }
    printf(">> %u digital output bit(s) registered\n", n_do);

    /* ===== Online ===== */
    if (ecrt_master_activate(master)) { fprintf(stderr, "activate failed\n"); goto out; }
    pd = ecrt_domain_data(domain);

    struct sched_param sp = { .sched_priority = 80 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp)) perror("sched_setscheduler");

    printf(">> cyclic start (%s mode), Ctrl+C to stop\n", mirror ? "mirror" : "walk");
    cyclic(mirror);

out:
    ecrt_release_master(master);
    return 0;
}
