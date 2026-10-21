#!/usr/bin/env python3
"""
benchmark-facets-monthend-surge.py
Simulates a Month-End Surge of 10,000 claims (20,000+ line items) dropping into ASQL.
Captures real-time empirical metrics across all 5 processing phases:
1. Ingestion / Staging Rate
2. Eligibility & Benefit Pricing Joins
3. Adjudication Rule Engine
4. State Update & Settlement Write   (claim headers AND claim lines, and the members' deductible accumulators)
5. Month-End Financial Balancing & 835 Remittance Generation

The rules, the write-back and the verification are the ones in test-facets-adjudication.py (loaded from there, so there is
one implementation): after the surge the results are read back out of ASQL and recomputed independently, and the script
exits non-zero if anything is wrong. The totals count what was written: an earlier version settled only the 10,000 claim
headers and left every one of the 20,000+ lines unadjudicated, so its claims-per-second figure excluded most of the work.
Time to build the synthetic batch in Python, and to verify, are reported on their own and are not part of the rate.

    ASQL_PASS_FILE / ASQL_SOCKET / ASQL_HOST / ASQL_PORT   where the server is (defaults: the local root)
    FACETS_SEED / FACETS_WRITE_MODE                        as in test-facets-adjudication.py
"""

import sys
import os
import time
import random
import importlib.util

# The rules, the write-back and the verification live in test-facets-adjudication.py (a hyphenated name, so loaded by path).
_spec = importlib.util.spec_from_file_location(
    "facets_adjudication", os.path.join(os.path.dirname(os.path.abspath(__file__)), "test-facets-adjudication.py"))
tf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tf)


def get_connection():
    """A connection to facets_sandbox (made, with its schema and reference data, when the server has none yet)."""
    conn = tf.get_connection()
    with conn.cursor() as cur:
        cur.execute("CREATE DATABASE IF NOT EXISTS facets_sandbox")
        cur.execute("USE facets_sandbox")
        cur.execute("SHOW TABLES")
        have = {r[0].upper() for r in cur.fetchall()}
    if not {"PDPD_PRODUCT", "MEME_MEMBER", "CLCL_CLAIM", "CDML_DETAIL", "PRCD_PROC_CODE", "PRPR_PROVIDER"} <= have:
        tf.setup_facets_schema(conn)
        tf.seed_facets_reference_data(conn)
    return conn


def run_monthend_surge(num_claims=10000):
    random.seed(int(os.environ.get("FACETS_SEED", "20261021")))
    mode = os.environ.get("FACETS_WRITE_MODE", "set")
    conn = get_connection()
    print("=" * 70)
    print(f" HEALTHCARE MONTH-END SURGE SIMULATION: {num_claims:,} CLAIMS")
    print("=" * 70)

    # Clean existing claims
    with conn.cursor() as cur:
        cur.execute("TRUNCATE TABLE CDML_DETAIL")
        cur.execute("TRUNCATE TABLE CLCL_CLAIM")

    # Generate synthetic month-end batch in memory (client side; not part of the rate)
    print(f"\n[PHASE 1] Ingestion: Staging {num_claims:,} raw EDI claims...")
    t_gen0 = time.time()
    cpt_pool = ["99213", "99214", "99215", "80053", "85025", "71046", "93000", "36415"]
    cpt_charges = {
        "99213": 135.00, "99214": 185.00, "99215": 250.00, "80053": 45.00,
        "85025": 30.00, "71046": 75.00, "93000": 55.00, "36415": 20.00
    }

    claims_data = []
    lines_data = []

    for i in range(1, num_claims + 1):
        clcl_id = f"261031{i:06d}"
        meme_ck = random.randint(1, 1000)
        prpr_id = f"PR{random.randint(1, 100):05d}"
        num_lines = random.randint(1, 3)
        chosen_cpts = random.sample(cpt_pool, num_lines)
        tot_chg = sum(cpt_charges[c] for c in chosen_cpts)

        claims_data.append((clcl_id, meme_ck, prpr_id, 'M', '01', tot_chg))
        for seq, cpt in enumerate(chosen_cpts, start=1):
            lines_data.append((clcl_id, seq, cpt, cpt_charges[cpt]))
    t_gen = time.time() - t_gen0

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
    t_ingest = time.time() - t0
    total_staged_rows = len(claims_data) + len(lines_data)
    ingest_throughput = total_staged_rows / t_ingest
    claims_per_sec_ingest = num_claims / t_ingest

    print(f"  -> Ingested {num_claims:,} claims & {len(lines_data):,} lines ({total_staged_rows:,} total rows)")
    print(f"  -> Time: {t_ingest:.2f} s ({claims_per_sec_ingest:,.0f} claims/sec | {ingest_throughput:,.0f} rows/sec)")
    print(f"  -> (building the batch in Python took {t_gen:.2f} s: not counted)")

    # Phase 2: In-RAM Eligibility & Pricing Joins
    print("\n[PHASE 2] Eligibility & Contract Pricing Joins...")
    t1 = time.time()
    with conn.cursor() as cur:
        plans, allowable, members = tf.load_reference(cur)

        cur.execute("""
            SELECT c.CLCL_ID, c.MEME_CK, l.CDML_SEQ_NO, l.PRCD_ID, l.CDML_CHG_AMT
            FROM CLCL_CLAIM c
            JOIN CDML_DETAIL l ON c.CLCL_ID = l.CLCL_ID
            WHERE c.CLCL_CUR_STS = '01'
            ORDER BY c.CLCL_ID, l.CDML_SEQ_NO
        """)
        raw_lines = cur.fetchall()
    t_join = time.time() - t1
    print(f"  -> Cross-table joins across Members, Plans, Fee Schedules & Lines: {t_join*1000:.1f} ms")

    # Phase 3: Adjudication Rule Logic Execution
    print("\n[PHASE 3] Adjudication Engine (Benefit Calculation & Rules)...")
    t2 = time.time()
    opening = {ck: m["ded_accum"] for ck, m in members.items()}
    line_updates, claim_updates = tf.adjudicate(raw_lines, plans, allowable, members)
    t_rules = time.time() - t2
    adjudication_rate = len(raw_lines) / t_rules
    print(f"  -> Adjudicated {len(raw_lines):,} lines across {num_claims:,} claims: {t_rules*1000:.1f} ms")
    print(f"  -> Pure Rule Engine Throughput: {adjudication_rate:,.0f} lines/sec ({num_claims/t_rules:,.0f} claims/sec)")

    # Phase 4: State Settlement / Commit: the lines, the claim headers and the members' accumulators
    print(f"\n[PHASE 4] State Settlement & Persistence (Writing Back to ASQL, '{mode}' mode)...")
    t3 = time.time()
    tf.write_back(conn, line_updates, claim_updates, mode)
    tf.persist_accumulators(conn, members, opening, mode)
    t_commit = time.time() - t3
    print(f"  -> Settled {len(line_updates):,} lines and {len(claim_updates):,} claims (Status '11' Paid): {t_commit*1000:.1f} ms")

    # Phase 5: Month-End Financial Audit & Remittance Reports
    print("\n[PHASE 5] Month-End Financial Reconciliation & 835 Remittance...")
    t4 = time.time()
    with conn.cursor() as cur:
        cur.execute("""
            SELECT 
                COUNT(*) as claim_count,
                SUM(CLCL_TOT_CHG) as total_billed,
                SUM(CLCL_TOT_ALLOW) as total_allowable,
                SUM(CLCL_TOT_PAYABLE) as plan_check_payout,
                SUM(CLCL_TOT_DED + CLCL_TOT_COPAY + CLCL_TOT_COINS) as member_out_of_pocket,
                SUM(CLCL_TOT_CHG - CLCL_TOT_ALLOW) as provider_writeoffs
            FROM CLCL_CLAIM
            WHERE CLCL_CUR_STS = '11'
        """)
        fin = cur.fetchone()
    t_fin = time.time() - t4

    print(f"  -> Financial Balancing Completed in: {t_fin*1000:.2f} ms")
    print(f"     • Claims Adjudicated: {fin[0]:,}")
    print(f"     • Total Billed Charges: ${fin[1]:,.2f}")
    print(f"     • Contract Allowable:   ${fin[2]:,.2f}")
    print(f"     • Insurer Payout (EFT): ${fin[3]:,.2f}")
    print(f"     • Member Responsibility:${fin[4]:,.2f}")
    print(f"     • Provider Write-Offs:  ${fin[5]:,.2f}")

    total_batch_time = t_ingest + t_join + t_rules + t_commit + t_fin
    avg_per_claim_ms = (total_batch_time / num_claims) * 1000.0

    # Verification: the results read back and recomputed independently (not part of the rate)
    print("\n[VERIFY] Recomputing every line from ASQL's own rows...")
    tv = time.time()
    failures = tf.verify_adjudication(conn, num_claims, len(lines_data), opening)
    t_verify = time.time() - tv
    print(f"  -> verification took {t_verify:.2f} s (not counted)")

    print("\n" + "=" * 70)
    print(f" SUMMARY METRICS FOR {num_claims:,} CLAIM MONTH-END SURGE" + ("" if not failures else "   *** VERIFICATION FAILED ***"))
    print("=" * 70)
    print(f"  Total Ingestion Time:       {t_ingest:.2f} s")
    print(f"  Total Adjudication Time:    {(t_join + t_rules + t_commit):.2f} s   (joins {t_join:.2f} + rules {t_rules:.2f} + settlement {t_commit:.2f})")
    print(f"  Month-end balancing:        {t_fin:.2f} s")
    print(f"  Total End-to-End Batch:     {total_batch_time:.2f} s   ({len(line_updates):,} lines, {len(claim_updates):,} claims settled)")
    print(f"  Average Latency Per Claim:  {avg_per_claim_ms:.2f} ms / claim")
    print(f"  Effective Processing Rate:  {num_claims / total_batch_time:,.0f} claims / sec")
    print(f"  Results verified:           {'YES' if not failures else 'NO: ' + str(len(failures)) + ' problem(s)'}")
    print("=" * 70)
    conn.close()
    if failures:
        sys.exit(1)


if __name__ == "__main__":
    run_monthend_surge(10000)
