#!/usr/bin/env python3
"""
test-facets-adjudication.py
Simulates a Cognizant TriZetto FACETS core schema and batch adjudication engine in ASQL.
Includes subscriber/member eligibility, provider networks, plan designs (PPO/HDHP),
deductible accumulator logic, copay/coinsurance splits, and CARC write-offs.

This is a test: it exits non-zero unless the stored results check out. After the adjudication cycle,
verify_adjudication() reads everything back out of ASQL, recomputes every claim line independently in exact decimal
arithmetic and checks the conservation rules (allowed = deductible + copay + coinsurance + plan paid, charge = allowed +
write-off, claim totals = sum of lines, no negative amounts, accumulators within the plan deductible, ASQL's own SUM /
COUNT agree with the rows). A last step corrupts one cent on one line and requires the verification to notice, so the
checker is itself tested. The data is seeded (FACETS_SEED, default fixed) so a failure reproduces.

    ASQL_PASS_FILE / ASQL_SOCKET / ASQL_HOST / ASQL_PORT   where the server is (defaults: the local root)
    FACETS_SEED        random seed (default 20261021)
    FACETS_WRITE_MODE  'set' (default): results written back with a staging table and one UPDATE ... JOIN per table;
                       'row': one UPDATE per line and per claim, as this script first did (every one a durable write)
"""

import sys
import os
import time
import random
from collections import defaultdict
from decimal import Decimal, ROUND_HALF_UP
import pymysql

# Where the ASQL server is: ASQL_PASS_FILE (required) holds the root password; ASQL_SOCKET (a path that does not
# exist, or unset, means TCP), ASQL_HOST and ASQL_PORT say how to reach it.
PASS_FILE = os.environ.get("ASQL_PASS_FILE", "")
SOCKET_FILE = os.environ.get("ASQL_SOCKET", "")
if not PASS_FILE:
    sys.exit("set ASQL_PASS_FILE to the ASQL root's mysql.pass (and ASQL_SOCKET to its mysql.sock)")
HOST = os.environ.get("ASQL_HOST", "127.0.0.1")
PORT = int(os.environ.get("ASQL_PORT", "3307"))

def get_connection(db=None):
    with open(PASS_FILE, "r") as f:
        pw = f.read().strip()
    if os.path.exists(SOCKET_FILE):
        return pymysql.connect(
            unix_socket=SOCKET_FILE,
            user="root",
            password=pw,
            database=db,
            autocommit=True
        )
    return pymysql.connect(
        host=HOST,
        port=PORT,
        user="root",
        password=pw,
        database=db,
        autocommit=True
    )

def setup_facets_schema(conn):
    print("==> Initializing TriZetto FACETS core schema in 'facets_sandbox'...")
    with conn.cursor() as cur:
        cur.execute("CREATE DATABASE IF NOT EXISTS facets_sandbox")
        cur.execute("USE facets_sandbox")

        cur.execute("DROP TABLE IF EXISTS CDML_DETAIL")
        cur.execute("DROP TABLE IF EXISTS CLCL_CLAIM")
        cur.execute("DROP TABLE IF EXISTS PRCD_PROC_CODE")
        cur.execute("DROP TABLE IF EXISTS PRPR_PROVIDER")
        cur.execute("DROP TABLE IF EXISTS MEME_MEMBER")
        cur.execute("DROP TABLE IF EXISTS SBSB_SUBSCRIBER")
        cur.execute("DROP TABLE IF EXISTS PDPD_PRODUCT")

        # 1. Product / Benefit Plan Design
        cur.execute("""
            CREATE TABLE PDPD_PRODUCT (
                PDPD_ID VARCHAR(8) PRIMARY KEY,
                PDPD_DESC VARCHAR(50),
                PDPD_DED_MAX DECIMAL(10,2),
                PDPD_COPAY DECIMAL(10,2),
                PDPD_COINS_PCT DECIMAL(5,2)
            )
        """)

        # 2. Subscriber Master
        cur.execute("""
            CREATE TABLE SBSB_SUBSCRIBER (
                SBSB_CK INT PRIMARY KEY,
                SBSB_ID VARCHAR(9) UNIQUE,
                SBSB_LAST_NAME VARCHAR(50),
                SBSB_FIRST_NAME VARCHAR(35),
                GRGR_ID VARCHAR(8)
            )
        """)

        # 3. Member / Dependent Master
        cur.execute("""
            CREATE TABLE MEME_MEMBER (
                MEME_CK INT PRIMARY KEY,
                SBSB_CK INT,
                MEME_REL_CODE VARCHAR(2),
                MEME_LAST_NAME VARCHAR(50),
                MEME_FIRST_NAME VARCHAR(35),
                PDPD_ID VARCHAR(8),
                MEME_DED_ACCUM DECIMAL(10,2) DEFAULT 0.00
            )
        """)

        # 4. Provider Master
        cur.execute("""
            CREATE TABLE PRPR_PROVIDER (
                PRPR_ID VARCHAR(12) PRIMARY KEY,
                PRPR_NPI VARCHAR(10),
                PRPR_NAME VARCHAR(70),
                PRPR_NET_STS VARCHAR(1)
            )
        """)

        # 5. Procedure Code & Fee Schedule
        cur.execute("""
            CREATE TABLE PRCD_PROC_CODE (
                PRCD_ID VARCHAR(10) PRIMARY KEY,
                PRCD_DESC VARCHAR(100),
                PRCD_ALLOW_AMT DECIMAL(10,2)
            )
        """)

        # 6. FACETS Claim Header (CLCL)
        cur.execute("""
            CREATE TABLE CLCL_CLAIM (
                CLCL_ID VARCHAR(12) PRIMARY KEY,
                MEME_CK INT,
                PRPR_ID VARCHAR(12),
                CLCL_CL_TYPE VARCHAR(1) DEFAULT 'M',
                CLCL_CUR_STS VARCHAR(2) DEFAULT '01',
                CLCL_TOT_CHG DECIMAL(10,2) DEFAULT 0.00,
                CLCL_TOT_ALLOW DECIMAL(10,2) DEFAULT 0.00,
                CLCL_TOT_PAYABLE DECIMAL(10,2) DEFAULT 0.00,
                CLCL_TOT_DED DECIMAL(10,2) DEFAULT 0.00,
                CLCL_TOT_COPAY DECIMAL(10,2) DEFAULT 0.00,
                CLCL_TOT_COINS DECIMAL(10,2) DEFAULT 0.00
            )
        """)

        # 7. FACETS Claim Line Detail (CDML)
        cur.execute("""
            CREATE TABLE CDML_DETAIL (
                CLCL_ID VARCHAR(12),
                CDML_SEQ_NO INT,
                PRCD_ID VARCHAR(10),
                CDML_CHG_AMT DECIMAL(10,2),
                CDML_ALLOW_AMT DECIMAL(10,2) DEFAULT 0.00,
                CDML_DISALL_AMT DECIMAL(10,2) DEFAULT 0.00,
                CDML_DED_AMT DECIMAL(10,2) DEFAULT 0.00,
                CDML_COPAY_AMT DECIMAL(10,2) DEFAULT 0.00,
                CDML_COINS_AMT DECIMAL(10,2) DEFAULT 0.00,
                CDML_PAID_AMT DECIMAL(10,2) DEFAULT 0.00,
                CDML_DISP VARCHAR(2) DEFAULT '01',
                CARC_CODE VARCHAR(10) DEFAULT '',
                PRIMARY KEY (CLCL_ID, CDML_SEQ_NO)
            )
        """)

def seed_facets_reference_data(conn):
    print("==> Seeding FACETS Benefit Plans, Providers, Members, and Fee Schedules...")

    # Products (PPO 500, PPO 1000, High Deductible Health Plan)
    products = [
        ("PPO500", "Gold PPO $500 Ded 80/20", 500.00, 25.00, 0.80),
        ("PPO1000", "Silver PPO $1000 Ded 80/20", 1000.00, 35.00, 0.80),
        ("HDHP2500", "Bronze HSA HDHP $2500 Ded 90/10", 2500.00, 0.00, 0.90)
    ]

    # Providers
    providers = [
        (f"PR{i:05d}", f"19827{i:05d}", f"Ohio Regional Clinic {i}", "I")
        for i in range(1, 101)
    ]

    # CPT Fee Schedule
    procedures = [
        ("99213", "Office Visit Low-Mod 20 min", 95.00),
        ("99214", "Office Visit Mod-High 30 min", 140.00),
        ("99215", "Office Visit High Complexity 45 min", 190.00),
        ("80053", "Comprehensive Metabolic Panel", 28.50),
        ("85025", "Complete Blood Count Automated", 18.25),
        ("71046", "Chest X-Ray 2 Views", 45.00),
        ("93000", "Electrocardiogram 12-lead ECG", 32.00),
        ("36415", "Venipuncture Routine Blood Draw", 12.00)
    ]

    # 1,000 Members across 800 Subscribers
    subscribers = []
    members = []
    p_choices = ["PPO500", "PPO1000", "HDHP2500"]

    for s_idx in range(1, 801):
        s_id = f"S{s_idx:08d}"
        subscribers.append((s_idx, s_id, f"LastName{s_idx}", f"SubFirstName{s_idx}", "GRP1001"))
        # Subscriber as Member
        plan = random.choice(p_choices)
        # Random initial YTD deductible accumulation ($0 to $400 already spent)
        prior_ded = round(random.uniform(0.0, 300.0), 2)
        members.append((s_idx, s_idx, "M", f"LastName{s_idx}", f"SubFirstName{s_idx}", plan, prior_ded))

    # Add 200 dependents (Spouse/Child)
    for d_idx in range(801, 1001):
        s_parent = random.randint(1, 800)
        rel = random.choice(["S", "C"])
        plan = random.choice(p_choices)
        members.append((d_idx, s_parent, rel, f"DepLastName{d_idx}", f"DepFirstName{d_idx}", plan, 0.00))

    with conn.cursor() as cur:
        cur.executemany("INSERT INTO PDPD_PRODUCT VALUES (%s, %s, %s, %s, %s)", products)
        cur.executemany("INSERT INTO PRPR_PROVIDER VALUES (%s, %s, %s, %s)", providers)
        cur.executemany("INSERT INTO PRCD_PROC_CODE VALUES (%s, %s, %s)", procedures)
        cur.executemany("INSERT INTO SBSB_SUBSCRIBER VALUES (%s, %s, %s, %s, %s)", subscribers)
        cur.executemany("INSERT INTO MEME_MEMBER VALUES (%s, %s, %s, %s, %s, %s, %s)", members)

    print(f"    Loaded {len(products)} products, {len(providers)} providers, {len(subscribers)} subscribers, {len(members)} members.")

def stage_facets_inbound_claims(conn, num_claims=1000):
    print(f"\n==> Staging {num_claims} unadjudicated claims into FACETS (Status '01' Entered)...")
    cpt_pool = ["99213", "99214", "99215", "80053", "85025", "71046", "93000", "36415"]
    cpt_charges = {
        "99213": 135.00, "99214": 185.00, "99215": 250.00, "80053": 45.00,
        "85025": 30.00, "71046": 75.00, "93000": 55.00, "36415": 20.00
    }

    claims_data = []
    lines_data = []

    for i in range(1, num_claims + 1):
        clcl_id = f"261021{i:06d}"
        meme_ck = random.randint(1, 1000)
        prpr_id = f"PR{random.randint(1, 100):05d}"

        num_lines = random.randint(1, 3)
        chosen_cpts = random.sample(cpt_pool, num_lines)
        tot_chg = sum(cpt_charges[c] for c in chosen_cpts)

        claims_data.append((clcl_id, meme_ck, prpr_id, 'M', '01', tot_chg))

        for seq, cpt in enumerate(chosen_cpts, start=1):
            lines_data.append((clcl_id, seq, cpt, cpt_charges[cpt]))

    t0 = time.time()
    with conn.cursor() as cur:
        cur.executemany(
            "INSERT INTO CLCL_CLAIM (CLCL_ID, MEME_CK, PRPR_ID, CLCL_CL_TYPE, CLCL_CUR_STS, CLCL_TOT_CHG) "
            "VALUES (%s, %s, %s, %s, %s, %s)",
            claims_data
        )
        cur.executemany(
            "INSERT INTO CDML_DETAIL (CLCL_ID, CDML_SEQ_NO, PRCD_ID, CDML_CHG_AMT) "
            "VALUES (%s, %s, %s, %s)",
            lines_data
        )
    dur = time.time() - t0
    total_staged = len(claims_data) + len(lines_data)
    print(f"    Staged {len(claims_data)} claims and {len(lines_data)} line items in {dur*1000:.1f} ms ({total_staged/dur:,.0f} rows/sec).")
    return len(claims_data), len(lines_data)

CENT = Decimal("0.01")


def money(x):
    """Round half up to the cent, as a ledger does (never a binary float)."""
    return Decimal(x).quantize(CENT, rounding=ROUND_HALF_UP)


def write_back(conn, line_updates, claim_updates, mode):
    """Write adjudicated lines and claims back. 'set': stage them in ASQL and apply with one UPDATE ... JOIN per table;
    'row': one UPDATE per line / claim (autocommit: each is its own durable write)."""
    with conn.cursor() as cur:
        if mode == "row":
            cur.executemany("""
                UPDATE CDML_DETAIL
                SET CDML_ALLOW_AMT = %s, CDML_DISALL_AMT = %s, CDML_DED_AMT = %s,
                    CDML_COPAY_AMT = %s, CDML_COINS_AMT = %s, CDML_PAID_AMT = %s,
                    CDML_DISP = %s, CARC_CODE = %s
                WHERE CLCL_ID = %s AND CDML_SEQ_NO = %s
            """, line_updates)
            cur.executemany("""
                UPDATE CLCL_CLAIM
                SET CLCL_TOT_ALLOW = %s, CLCL_TOT_PAYABLE = %s, CLCL_TOT_DED = %s,
                    CLCL_TOT_COPAY = %s, CLCL_TOT_COINS = %s, CLCL_CUR_STS = %s
                WHERE CLCL_ID = %s
            """, claim_updates)
            return
        cur.execute("DROP TABLE IF EXISTS stage_line_updates")
        cur.execute("""
            CREATE TABLE stage_line_updates (
                CLCL_ID VARCHAR(12), CDML_SEQ_NO INT,
                allow_amt DECIMAL(10,2), disall_amt DECIMAL(10,2), ded_amt DECIMAL(10,2), copay_amt DECIMAL(10,2),
                coins_amt DECIMAL(10,2), paid_amt DECIMAL(10,2), disp VARCHAR(2), carc VARCHAR(10),
                PRIMARY KEY (CLCL_ID, CDML_SEQ_NO)
            )
        """)
        cur.executemany(
            "INSERT INTO stage_line_updates VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
            [(u[8], u[9], u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7]) for u in line_updates])
        cur.execute("""
            UPDATE CDML_DETAIL l JOIN stage_line_updates s ON l.CLCL_ID = s.CLCL_ID AND l.CDML_SEQ_NO = s.CDML_SEQ_NO
            SET l.CDML_ALLOW_AMT = s.allow_amt, l.CDML_DISALL_AMT = s.disall_amt, l.CDML_DED_AMT = s.ded_amt,
                l.CDML_COPAY_AMT = s.copay_amt, l.CDML_COINS_AMT = s.coins_amt, l.CDML_PAID_AMT = s.paid_amt,
                l.CDML_DISP = s.disp, l.CARC_CODE = s.carc
        """)
        cur.execute("DROP TABLE stage_line_updates")
        cur.execute("DROP TABLE IF EXISTS stage_claim_updates")
        cur.execute("""
            CREATE TABLE stage_claim_updates (
                CLCL_ID VARCHAR(12) PRIMARY KEY,
                tot_allow DECIMAL(10,2), tot_payable DECIMAL(10,2), tot_ded DECIMAL(10,2),
                tot_copay DECIMAL(10,2), tot_coins DECIMAL(10,2), status VARCHAR(2)
            )
        """)
        cur.executemany(
            "INSERT INTO stage_claim_updates VALUES (%s, %s, %s, %s, %s, %s, %s)",
            [(u[6], u[0], u[1], u[2], u[3], u[4], u[5]) for u in claim_updates])
        cur.execute("""
            UPDATE CLCL_CLAIM c JOIN stage_claim_updates s ON c.CLCL_ID = s.CLCL_ID
            SET c.CLCL_TOT_ALLOW = s.tot_allow, c.CLCL_TOT_PAYABLE = s.tot_payable, c.CLCL_TOT_DED = s.tot_ded,
                c.CLCL_TOT_COPAY = s.tot_copay, c.CLCL_TOT_COINS = s.tot_coins, c.CLCL_CUR_STS = s.status
        """)
        cur.execute("DROP TABLE stage_claim_updates")


def persist_accumulators(conn, members, opening, mode):
    """The deductible each member has met is state, not a local variable: without it the next batch starts over."""
    changed = [(m["ded_accum"], ck) for ck, m in members.items() if m["ded_accum"] != opening[ck]]
    with conn.cursor() as cur:
        if mode == "row":
            cur.executemany("UPDATE MEME_MEMBER SET MEME_DED_ACCUM = %s WHERE MEME_CK = %s", changed)
            return len(changed)
        cur.execute("DROP TABLE IF EXISTS stage_accum")
        cur.execute("CREATE TABLE stage_accum (MEME_CK INT PRIMARY KEY, accum DECIMAL(10,2))")
        cur.executemany("INSERT INTO stage_accum VALUES (%s, %s)", [(ck, a) for a, ck in changed])
        cur.execute("UPDATE MEME_MEMBER m JOIN stage_accum s ON m.MEME_CK = s.MEME_CK SET m.MEME_DED_ACCUM = s.accum")
        cur.execute("DROP TABLE stage_accum")
    return len(changed)


def adjudicate(raw_lines, plans, allowable, members):
    """The rules, in exact decimal arithmetic. raw_lines are (claim, member, seq, procedure, charge) in claim / line
    order; members' accumulators are drawn down as they go. Returns (line_updates, claim_updates)."""
    line_updates = []
    claim_aggregates = {}  # clcl_id -> [tot_allow, tot_payable, tot_ded, tot_copay, tot_coins]
    for clcl_id, meme_ck, seq_no, prcd_id, chg_amt in raw_lines:
        chg = Decimal(str(chg_amt))
        allow = allowable.get(prcd_id, money(chg * Decimal("0.70")))
        disallow = max(Decimal("0"), money(chg - allow))

        mbr = members.get(meme_ck, {"plan": "PPO500", "ded_accum": Decimal("0")})
        plan = plans[mbr["plan"]]

        # Deductible check
        rem_ded = max(Decimal("0"), plan["ded_max"] - mbr["ded_accum"])
        ded_applied = min(allow, rem_ded)
        mbr["ded_accum"] += ded_applied

        # Copay check (office visits 99213, 99214, 99215)
        after_ded = allow - ded_applied
        copay_applied = Decimal("0")
        if prcd_id.startswith("992") and after_ded > 0:
            copay_applied = min(after_ded, plan["copay"])

        # Coinsurance split (plan share / member share)
        subject_to_coins = max(Decimal("0"), after_ded - copay_applied)
        plan_paid = money(subject_to_coins * plan["coins_pct"])
        member_coins = money(subject_to_coins - plan_paid)

        carc = "CO-45" if disallow > 0 else ""

        line_updates.append((
            allow, disallow, money(ded_applied), money(copay_applied), member_coins, plan_paid, "11", carc,
            clcl_id, seq_no
        ))

        if clcl_id not in claim_aggregates:
            claim_aggregates[clcl_id] = [Decimal("0")] * 5
        agg = claim_aggregates[clcl_id]
        agg[0] += allow
        agg[1] += plan_paid
        agg[2] += money(ded_applied)
        agg[3] += money(copay_applied)
        agg[4] += member_coins

    claim_updates = [
        (t[0], t[1], t[2], t[3], t[4], "11", clcl_id)
        for clcl_id, t in claim_aggregates.items()
    ]
    return line_updates, claim_updates


def load_reference(cur):
    cur.execute("SELECT PDPD_ID, PDPD_DED_MAX, PDPD_COPAY, PDPD_COINS_PCT FROM PDPD_PRODUCT")
    plans = {r[0]: {"ded_max": Decimal(str(r[1])), "copay": Decimal(str(r[2])), "coins_pct": Decimal(str(r[3]))} for r in cur.fetchall()}
    cur.execute("SELECT PRCD_ID, PRCD_ALLOW_AMT FROM PRCD_PROC_CODE")
    allowable = {r[0]: Decimal(str(r[1])) for r in cur.fetchall()}
    cur.execute("SELECT MEME_CK, PDPD_ID, MEME_DED_ACCUM FROM MEME_MEMBER")
    members = {r[0]: {"plan": r[1], "ded_accum": Decimal(str(r[2]))} for r in cur.fetchall()}
    return plans, allowable, members


def run_facets_adjudication_cycle(conn):
    mode = os.environ.get("FACETS_WRITE_MODE", "set")
    print("\n==> EXECUTING FACETS ADJUDICATION CYCLE IN ASQL...")
    print("    Rules: Benefit determination, In-Network Fee Schedule pricing,")
    print("           Deductible accumulator drawdown, Copay subtraction, 80/20 Coinsurance split.")
    print(f"    Money is exact decimal; results are written back in '{mode}' mode.")

    t0 = time.time()

    # Step 1: Pre-fetch in-RAM benefit and pricing dictionaries for high-speed rule engine
    with conn.cursor() as cur:
        plans, allowable, members = load_reference(cur)

        # Fetch unadjudicated claims with their detail lines
        cur.execute("""
            SELECT c.CLCL_ID, c.MEME_CK, l.CDML_SEQ_NO, l.PRCD_ID, l.CDML_CHG_AMT
            FROM CLCL_CLAIM c
            JOIN CDML_DETAIL l ON c.CLCL_ID = l.CLCL_ID
            WHERE c.CLCL_CUR_STS = '01'
            ORDER BY c.CLCL_ID, l.CDML_SEQ_NO
        """)
        raw_lines = cur.fetchall()

    t_fetch = time.time() - t0

    # Step 2: Adjudicate each claim and line item in-memory
    t_adj_start = time.time()
    opening = {ck: m["ded_accum"] for ck, m in members.items()}   # what each member had met before this batch
    line_updates, claim_updates = adjudicate(raw_lines, plans, allowable, members)
    t_adj = time.time() - t_adj_start

    # Step 3: Write back adjudicated results to ASQL
    t_write_start = time.time()
    write_back(conn, line_updates, claim_updates, mode)
    persist_accumulators(conn, members, opening, mode)
    t_write = time.time() - t_write_start
    total_cycle = time.time() - t0

    print(f"    Cycle Completed in {total_cycle*1000:.1f} ms!")
    print(f"      - Query & Join Phase: {t_fetch*1000:.1f} ms")
    print(f"      - Adjudication Rule Logic: {t_adj*1000:.1f} ms")
    print(f"      - Batch Update Commit: {t_write*1000:.1f} ms ({len(line_updates) + len(claim_updates)} records)")
    return len(claim_updates), len(line_updates), t_write, opening


def run_facets_financial_reporting(conn):
    print("\n==> EXECUTING FACETS FINANCIAL & REMITTANCE AUDIT...")

    queries = [
        (
            "1. Financial Summary: Plan Paid vs Member Liability vs Provider Write-off",
            """
            SELECT 
                COUNT(*) as adjudicated_claims,
                SUM(CLCL_TOT_CHG) as total_billed_charges,
                SUM(CLCL_TOT_ALLOW) as total_contract_allowable,
                SUM(CLCL_TOT_PAYABLE) as total_plan_paid,
                SUM(CLCL_TOT_DED) as member_deductible_paid,
                SUM(CLCL_TOT_COPAY) as member_copays_paid,
                SUM(CLCL_TOT_COINS) as member_coinsurance_paid,
                SUM(CLCL_TOT_CHG - CLCL_TOT_ALLOW) as provider_contractual_writeoffs
            FROM CLCL_CLAIM
            WHERE CLCL_CUR_STS = '11'
            """
        ),
        (
            "2. Top 5 Provider Check Remittance (835 Electronic Remittance Advice):",
            """
            SELECT 
                p.PRPR_NAME,
                p.PRPR_NPI,
                COUNT(c.CLCL_ID) as paid_claim_count,
                SUM(c.CLCL_TOT_PAYABLE) as total_remittance_check_amt
            FROM CLCL_CLAIM c
            JOIN PRPR_PROVIDER p ON c.PRPR_ID = p.PRPR_ID
            WHERE c.CLCL_CUR_STS = '11'
            GROUP BY p.PRPR_NAME, p.PRPR_NPI
            ORDER BY total_remittance_check_amt DESC
            LIMIT 5
            """
        ),
        (
            "3. Plan Cost Breakdown by Benefit Product (PPO vs HDHP):",
            """
            SELECT 
                prod.PDPD_DESC,
                COUNT(c.CLCL_ID) as claim_volume,
                SUM(c.CLCL_TOT_PAYABLE) as plan_exposure,
                ROUND(AVG(c.CLCL_TOT_DED + c.CLCL_TOT_COPAY + c.CLCL_TOT_COINS), 2) as avg_member_cost
            FROM CLCL_CLAIM c
            JOIN MEME_MEMBER m ON c.MEME_CK = m.MEME_CK
            JOIN PDPD_PRODUCT prod ON m.PDPD_ID = prod.PDPD_ID
            GROUP BY prod.PDPD_DESC
            """
        )
    ]

    with conn.cursor() as cur:
        for title, sql in queries:
            t0 = time.time()
            cur.execute(sql)
            rows = cur.fetchall()
            lat = (time.time() - t0) * 1000.0

            print(f"\n--- {title} ({lat:.2f} ms) ---")
            col_names = [desc[0] for desc in cur.description]
            print("  |  ".join(col_names))
            print("-" * 75)
            for r in rows:
                print("  |  ".join(f"{float(v):,.2f}" if isinstance(v, (int, float)) and not isinstance(v, bool) and '.' in str(v) else str(v) for v in r))

def verify_adjudication(conn, expect_claims, expect_lines, opening, label=""):
    """Read the results back out of ASQL and check them (see the module docstring). Returns the list of failures
    (empty = every check passed); prints a summary and the first few failures."""
    fails = []
    checks = [0]

    def check(ok, what):
        checks[0] += 1
        if not ok:
            fails.append(what)

    with conn.cursor() as cur:
        plans, fee, members = load_reference(cur)
        cur.execute("SELECT CLCL_ID, MEME_CK, CLCL_CUR_STS, CLCL_TOT_CHG, CLCL_TOT_ALLOW, CLCL_TOT_PAYABLE, CLCL_TOT_DED, "
                    "CLCL_TOT_COPAY, CLCL_TOT_COINS FROM CLCL_CLAIM ORDER BY CLCL_ID")
        claims = cur.fetchall()
        cur.execute("SELECT CLCL_ID, CDML_SEQ_NO, PRCD_ID, CDML_CHG_AMT, CDML_ALLOW_AMT, CDML_DISALL_AMT, CDML_DED_AMT, "
                    "CDML_COPAY_AMT, CDML_COINS_AMT, CDML_PAID_AMT, CDML_DISP, CARC_CODE FROM CDML_DETAIL "
                    "ORDER BY CLCL_ID, CDML_SEQ_NO")
        lines = cur.fetchall()

    check(len(claims) == expect_claims, f"claim count {len(claims)} != staged {expect_claims}")
    check(len(lines) == expect_lines, f"line count {len(lines)} != staged {expect_lines}")

    by_claim = defaultdict(list)
    for ln in lines:
        by_claim[ln[0]].append(ln)
    claim_member = {c[0]: c[1] for c in claims}

    # Independent recomputation, in the order the adjudicator works (claim id, then line), from the stored charges.
    accum = dict(opening)
    ded_taken = defaultdict(lambda: Decimal("0"))
    exp_claim = {}
    for cid in sorted(by_claim):
        ck = claim_member.get(cid)
        plan = plans[members[ck]["plan"] if ck in members else "PPO500"]
        tot = [Decimal("0")] * 5
        for (_, seq, prcd, chg, allow, disall, ded, cp, cn, paid, disp, carc) in by_claim[cid]:
            chg, allow, disall, ded, cp, cn, paid = (Decimal(str(v)) for v in (chg, allow, disall, ded, cp, cn, paid))
            tag = f"{cid}/{seq}"
            e_allow = fee.get(prcd, money(chg * Decimal("0.70")))
            e_disall = max(Decimal("0"), money(chg - e_allow))
            rem = max(Decimal("0"), plan["ded_max"] - accum.get(ck, Decimal("0")))
            e_ded = min(e_allow, rem)
            accum[ck] = accum.get(ck, Decimal("0")) + e_ded
            ded_taken[ck] += e_ded
            after = e_allow - e_ded
            e_cp = min(after, plan["copay"]) if prcd.startswith("992") and after > 0 else Decimal("0")
            subj = max(Decimal("0"), after - e_cp)
            e_paid = money(subj * plan["coins_pct"])
            e_cn = money(subj - e_paid)
            check(allow == e_allow, f"{tag} allowed {allow}, expected {e_allow}")
            check(disall == e_disall, f"{tag} disallowed {disall}, expected {e_disall}")
            check(ded == money(e_ded), f"{tag} deductible {ded}, expected {money(e_ded)}")
            check(cp == money(e_cp), f"{tag} copay {cp}, expected {money(e_cp)}")
            check(paid == e_paid, f"{tag} plan paid {paid}, expected {e_paid}")
            check(cn == e_cn, f"{tag} coinsurance {cn}, expected {e_cn}")
            # whatever the rules say, the money is conserved
            check(allow == ded + cp + cn + paid,
                  f"{tag} allowed {allow} != deductible {ded} + copay {cp} + coinsurance {cn} + plan paid {paid}")
            check(chg == allow + disall, f"{tag} charge {chg} != allowed {allow} + disallowed {disall}")
            check(min(allow, disall, ded, cp, cn, paid) >= 0, f"{tag} has a negative amount")
            check((carc == "CO-45") == (disall > 0), f"{tag} CARC {carc!r} with a write-off of {disall}")
            check(disp == "11", f"{tag} line disposition {disp!r}, not '11'")
            for i, v in enumerate((allow, paid, ded, cp, cn)):
                tot[i] += v
        exp_claim[cid] = tot

    for (cid, ck, sts, chg, allow, paid, ded, cp, cn) in claims:
        if cid not in exp_claim:
            check(False, f"{cid} has no lines")
            continue
        chg, allow, paid, ded, cp, cn = (Decimal(str(v)) for v in (chg, allow, paid, ded, cp, cn))
        check(sts == "11", f"{cid} status {sts!r}, not '11'")
        check((allow, paid, ded, cp, cn) == tuple(exp_claim[cid]),
              f"{cid} totals {(allow, paid, ded, cp, cn)} != the sum of its lines {tuple(exp_claim[cid])}")
        check(chg == sum((Decimal(str(l[3])) for l in by_claim[cid]), Decimal("0")), f"{cid} billed {chg} != the sum of its line charges")

    # the deductible a member has met: never past the plan's, and saved for the next batch
    for ck, m in members.items():
        ded_max = plans[m["plan"]]["ded_max"]
        check(opening[ck] + ded_taken[ck] <= max(ded_max, opening[ck]),
              f"member {ck} met {opening[ck] + ded_taken[ck]} of a {ded_max} deductible")
        check(m["ded_accum"] == opening[ck] + ded_taken[ck],
              f"member {ck} accumulator saved as {m['ded_accum']}, expected {opening[ck] + ded_taken[ck]}")

    # ASQL's own aggregates against the same sums taken here
    with conn.cursor() as cur:
        cur.execute("SELECT COUNT(*), SUM(CLCL_TOT_CHG), SUM(CLCL_TOT_ALLOW), SUM(CLCL_TOT_PAYABLE), SUM(CLCL_TOT_DED), "
                    "SUM(CLCL_TOT_COPAY), SUM(CLCL_TOT_COINS) FROM CLCL_CLAIM WHERE CLCL_CUR_STS = '11'")
        a = cur.fetchone()
        cur.execute("SELECT COUNT(*), SUM(CDML_PAID_AMT), SUM(CDML_ALLOW_AMT) FROM CDML_DETAIL")
        b = cur.fetchone()
        cur.execute("SELECT COUNT(*) FROM CLCL_CLAIM WHERE CLCL_CUR_STS <> '11'")
        pending = cur.fetchone()[0]
    mine = [sum((Decimal(str(c[i])) for c in claims), Decimal("0")) for i in (3, 4, 5, 6, 7, 8)]
    check(a[0] == len(claims), f"ASQL counted {a[0]} paid claims, read {len(claims)}")
    for i, name in enumerate(("billed", "allowed", "plan paid", "deductible", "copay", "coinsurance")):
        check(Decimal(str(a[i + 1])) == mine[i], f"ASQL SUM {name} {a[i + 1]} != {mine[i]}, summed from the rows")
    check(Decimal(str(b[1])) == sum((Decimal(str(l[9])) for l in lines), Decimal("0")), f"ASQL SUM of line paid {b[1]} disagrees with the rows")
    check(Decimal(str(b[2])) == sum((Decimal(str(l[4])) for l in lines), Decimal("0")), f"ASQL SUM of line allowed {b[2]} disagrees with the rows")
    check(pending == 0, f"{pending} claims are not in status 11")

    print(f"    verification{(' ' + label) if label else ''}: {checks[0]:,} checks over {len(claims):,} claims / {len(lines):,} lines: "
          f"{'all passed' if not fails else str(len(fails)) + ' FAILED'}")
    for f in fails[:20]:
        print("      FAIL:", f)
    if len(fails) > 20:
        print(f"      ... and {len(fails) - 20} more")
    return fails


def verify_detects_tamper(conn, expect_claims, expect_lines, opening):
    """The checker is tested too: move one cent from one line's coinsurance to its plan-paid and require the checks to
    notice (it still adds up per line only if the other fields move with it - they do not), then put it back."""
    with conn.cursor() as cur:
        cur.execute("SELECT CLCL_ID, CDML_SEQ_NO, CDML_PAID_AMT FROM CDML_DETAIL WHERE CDML_PAID_AMT > 0 ORDER BY CLCL_ID, CDML_SEQ_NO LIMIT 1")
        row = cur.fetchone()
        if row is None:
            return ["no paid line to tamper with"]
        cid, seq, paid = row[0], row[1], Decimal(str(row[2]))
        cur.execute("UPDATE CDML_DETAIL SET CDML_PAID_AMT = %s WHERE CLCL_ID = %s AND CDML_SEQ_NO = %s", (paid + CENT, cid, seq))
    print(f"    tamper test: one cent added to {cid}/{seq}'s plan paid ...")
    noticed = verify_adjudication(conn, expect_claims, expect_lines, opening, "(tampered)")
    with conn.cursor() as cur:
        cur.execute("UPDATE CDML_DETAIL SET CDML_PAID_AMT = %s WHERE CLCL_ID = %s AND CDML_SEQ_NO = %s", (paid, cid, seq))
    return [] if noticed else ["a one-cent change to a stored line went unnoticed by verify_adjudication"]


def main():
    random.seed(int(os.environ.get("FACETS_SEED", "20261021")))
    conn = get_connection()
    failures = []
    try:
        setup_facets_schema(conn)
        seed_facets_reference_data(conn)
        num_claims, num_lines = stage_facets_inbound_claims(conn, num_claims=1000)
        n_claims, n_lines, _, opening = run_facets_adjudication_cycle(conn)
        run_facets_financial_reporting(conn)

        print("\n==> VERIFYING THE ADJUDICATION (independent recomputation from ASQL's own rows)...")
        if (n_claims, n_lines) != (num_claims, num_lines):
            failures.append(f"adjudicated {n_claims} claims / {n_lines} lines, staged {num_claims} / {num_lines}")
        failures += verify_adjudication(conn, num_claims, num_lines, opening)
        failures += verify_detects_tamper(conn, num_claims, num_lines, opening)
        failures += verify_adjudication(conn, num_claims, num_lines, opening, "(restored)")

        print("\n========================================================")
        if failures:
            print(f" FAILED: {len(failures)} problem(s) in the FACETS adjudication on ASQL")
            print("========================================================")
            sys.exit(1)
        print(" PASSED: FACETS model adjudication verified on ASQL (recomputed, conserved, and the checker catches tampering)")
        print("========================================================")
    finally:
        conn.close()


if __name__ == "__main__":
    main()
