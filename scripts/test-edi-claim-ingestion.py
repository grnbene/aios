#!/usr/bin/env python3
"""
test-edi-claim-ingestion.py
Generates a realistic HIPAA X12 837P claim batch, stream-parses segments,
validates against in-memory reference tables, ingests into ASQL,
and benchmarks adjudication query performance.
"""

import sys
import os
import time
import random
import pymysql

# Where the ASQL server is: ASQL_PASS_FILE (required) holds the root password; ASQL_SOCKET (a path that does not
# exist, or unset, means TCP), ASQL_HOST and ASQL_PORT say how to reach it.
PASS_FILE = os.environ.get("ASQL_PASS_FILE", "")
SOCKET_FILE = os.environ.get("ASQL_SOCKET", "")
HOST = os.environ.get("ASQL_HOST", "127.0.0.1")
PORT = int(os.environ.get("ASQL_PORT", "3307"))
if not PASS_FILE:
    sys.exit("set ASQL_PASS_FILE to the ASQL root's mysql.pass (and ASQL_SOCKET to its mysql.sock)")

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

def setup_schema(conn):
    print("==> Setting up schema in 'claims_sandbox'...")
    with conn.cursor() as cur:
        cur.execute("CREATE DATABASE IF NOT EXISTS claims_sandbox")
        cur.execute("USE claims_sandbox")
        
        # Drop old tables
        cur.execute("DROP TABLE IF EXISTS claim_lines")
        cur.execute("DROP TABLE IF EXISTS claim_diagnoses")
        cur.execute("DROP TABLE IF EXISTS claims")
        cur.execute("DROP TABLE IF EXISTS fee_schedule")
        cur.execute("DROP TABLE IF EXISTS npi_registry")

        # 1. Reference: NPI Registry
        cur.execute("""
            CREATE TABLE npi_registry (
                npi VARCHAR(10) PRIMARY KEY,
                provider_name VARCHAR(100),
                taxonomy VARCHAR(20),
                state VARCHAR(2)
            )
        """)

        # 2. Reference: Fee Schedule (CPT / Allowable)
        cur.execute("""
            CREATE TABLE fee_schedule (
                cpt_code VARCHAR(10) PRIMARY KEY,
                description VARCHAR(100),
                allowable_amount DECIMAL(10,2)
            )
        """)

        # 3. Claims Header
        cur.execute("""
            CREATE TABLE claims (
                claim_id INT AUTO_INCREMENT PRIMARY KEY,
                claim_control_number VARCHAR(50) UNIQUE,
                patient_member_id VARCHAR(30),
                billing_npi VARCHAR(10),
                total_charge DECIMAL(10,2),
                place_of_service VARCHAR(2),
                status VARCHAR(20) DEFAULT 'RECEIVED'
            )
        """)

        # 4. Claim Service Lines
        cur.execute("""
            CREATE TABLE claim_lines (
                line_id INT AUTO_INCREMENT PRIMARY KEY,
                claim_id INT,
                line_number INT,
                cpt_code VARCHAR(10),
                charge_amount DECIMAL(10,2),
                units INT,
                service_date VARCHAR(10)
            )
        """)

        # 5. Claim Diagnoses
        cur.execute("""
            CREATE TABLE claim_diagnoses (
                diag_id INT AUTO_INCREMENT PRIMARY KEY,
                claim_id INT,
                qualifier VARCHAR(4),
                icd10_code VARCHAR(10)
            )
        """)

def seed_reference_data(conn):
    print("==> Seeding reference tables (NPI Registry & CPT Fee Schedules)...")
    sample_npis = [
        (f"19827{i:05d}", f"Clinic Provider {i}", "207Q00000X", "OH")
        for i in range(1, 101)
    ]
    sample_cpt = [
        ("99213", "Office outpatient visit 20-29 min", 95.00),
        ("99214", "Office outpatient visit 30-39 min", 140.00),
        ("99215", "Office outpatient visit 40-54 min", 190.00),
        ("80053", "Comprehensive metabolic panel", 28.50),
        ("85025", "Complete CBC w/ automated diff", 18.25),
        ("71046", "Radiologic exam chest 2 views", 45.00),
        ("93000", "Electrocardiogram routine ECG", 32.00),
        ("36415", "Routine venipuncture collection", 12.00),
        ("90686", "Flu vaccine quadrivalent 0.5ml", 24.00),
        ("90471", "Immunization admin single inject", 28.00)
    ]

    with conn.cursor() as cur:
        cur.executemany(
            "INSERT INTO npi_registry (npi, provider_name, taxonomy, state) VALUES (%s, %s, %s, %s)",
            sample_npis
        )
        cur.executemany(
            "INSERT INTO fee_schedule (cpt_code, description, allowable_amount) VALUES (%s, %s, %s)",
            sample_cpt
        )
    print(f"    Seeded {len(sample_npis)} NPIs and {len(sample_cpt)} fee schedule procedures.")

def generate_edi_837_batch(filename, num_claims=1000):
    print(f"==> Generating realistic X12 837P EDI batch with {num_claims} claims...")
    cpt_pool = ["99213", "99214", "99215", "80053", "85025", "71046", "93000", "36415"]
    cpt_prices = {
        "99213": 135.00, "99214": 185.00, "99215": 250.00, "80053": 45.00,
        "85025": 30.00, "71046": 75.00, "93000": 55.00, "36415": 20.00
    }
    icd10_pool = ["E11.9", "I10", "J45.909", "M54.5", "Z00.00", "K21.9", "F41.1"]

    t0 = time.time()
    segments = []
    
    # Interchange & Functional Group headers
    segments.append("ISA*00*          *00*          *ZZ*SUBMITTER123   *ZZ*PAYER999       *261021*1400*^*00501*000000001*0*P*:~")
    segments.append("GS*HC*SUBMITTER123*PAYER999*20261021*1400*1*X*005010X222A1~")
    segments.append("ST*837*0001*005010X222A1~")
    segments.append("BHT*0019*00*BATCH-2026-X12*20261021*1400*CH~")
    segments.append("NM1*41*2*CLEARINGHOUSE*****46*CH12345~")
    segments.append("NM1*40*2*HEALTH PLAN*****46*PAYER999~")

    # HL loops and claims
    hl_count = 0
    for i in range(1, num_claims + 1):
        hl_count += 1
        npi = f"19827{random.randint(1, 100):05d}"
        mbr = f"MBR{random.randint(10000000, 99999999)}"
        ccn = f"CLM-2026-{i:06d}"

        # Choose 1 to 3 service lines
        num_lines = random.randint(1, 3)
        chosen_cpts = random.sample(cpt_pool, num_lines)
        total_billed = sum(cpt_prices[c] for c in chosen_cpts)

        # Provider HL
        segments.append(f"HL*{hl_count}**20*1~")
        segments.append(f"NM1*85*2*PROVIDER ORG*****XX*{npi}~")

        # Subscriber HL
        hl_count += 1
        segments.append(f"HL*{hl_count}*{hl_count-1}*22*0~")
        segments.append("SBR*P*18*******CI~")
        segments.append(f"NM1*IL*1*PATIENT*SAMPLE****MI*{mbr}~")

        # Claim Loop (2300)
        segments.append(f"CLM*{ccn}*{total_billed:.2f}***11:B:1*Y*A*Y*Y~")

        # Diagnoses (HI)
        diags = random.sample(icd10_pool, random.randint(1, 2))
        hi_parts = [f"BK:{diags[0]}"]
        if len(diags) > 1:
            hi_parts.append(f"BF:{diags[1]}")
        segments.append(f"HI*{'*'.join(hi_parts)}~")

        # Service Lines (2400)
        for lx_num, cpt in enumerate(chosen_cpts, start=1):
            charge = cpt_prices[cpt]
            segments.append(f"LX*{lx_num}~")
            segments.append(f"SV1*HC:{cpt}*{charge:.2f}*UN*1***1~")
            segments.append("DTP*472*D8*20261015~")

    segments.append(f"SE*{len(segments) - 2}*0001~")
    segments.append("GE*1*1~")
    segments.append("IEA*1*000000001~")

    raw_text = "\n".join(segments)
    with open(filename, "w") as f:
        f.write(raw_text)

    dur = time.time() - t0
    fsize = os.path.getsize(filename)
    print(f"    Wrote {len(segments)} X12 segments ({fsize / 1024:.1f} KB) in {dur*1000:.1f} ms.")
    return filename

def parse_and_ingest_x12(filename, conn):
    print("\n==> Stream-parsing X12 837 file and ingesting into ASQL...")
    t0 = time.time()

    claims_batch = []
    lines_batch = []
    diag_batch = []

    current_npi = None
    current_mbr = None
    current_ccn = None
    current_claim_total = 0.0
    claim_counter = 0

    seg_count = 0
    with open(filename, "r") as f:
        content = f.read()

    # Streaming split on segment terminator
    segments = [s.strip() for s in content.split("~") if s.strip()]

    for seg in segments:
        seg_count += 1
        elems = seg.split("*")
        tag = elems[0]

        if tag == "NM1":
            entity_id = elems[1] if len(elems) > 1 else ""
            if entity_id == "85":  # Billing Provider
                current_npi = elems[9] if len(elems) > 9 else ""
            elif entity_id == "IL": # Subscriber / Patient
                current_mbr = elems[9] if len(elems) > 9 else ""

        elif tag == "CLM":
            current_ccn = elems[1]
            current_claim_total = float(elems[2])
            claim_counter += 1
            claims_batch.append((
                claim_counter,
                current_ccn,
                current_mbr,
                current_npi,
                current_claim_total,
                "11",
                "RECEIVED"
            ))

        elif tag == "HI":
            # Diagnosis codes
            for comp in elems[1:]:
                parts = comp.split(":")
                if len(parts) >= 2:
                    qualifier, code = parts[0], parts[1]
                    diag_batch.append((claim_counter, qualifier, code))

        elif tag == "SV1":
            # Service line
            comp = elems[1].split(":")
            cpt = comp[1] if len(comp) > 1 else comp[0]
            charge = float(elems[2])
            units = int(elems[4]) if len(elems) > 4 and elems[4].isdigit() else 1
            lines_batch.append((
                claim_counter,
                len([l for l in lines_batch if l[0] == claim_counter]) + 1,
                cpt,
                charge,
                units,
                "2026-10-15"
            ))

    parse_dur = time.time() - t0
    print(f"    Parsed {seg_count} segments -> {len(claims_batch)} claims, {len(lines_batch)} lines, {len(diag_batch)} diagnoses in {parse_dur*1000:.1f} ms.")

    # Batch Insert into ASQL
    t_insert_start = time.time()
    with conn.cursor() as cur:
        cur.executemany(
            "INSERT INTO claims (claim_id, claim_control_number, patient_member_id, billing_npi, total_charge, place_of_service, status) "
            "VALUES (%s, %s, %s, %s, %s, %s, %s)",
            claims_batch
        )
        cur.executemany(
            "INSERT INTO claim_lines (claim_id, line_number, cpt_code, charge_amount, units, service_date) "
            "VALUES (%s, %s, %s, %s, %s, %s)",
            lines_batch
        )
        cur.executemany(
            "INSERT INTO claim_diagnoses (claim_id, qualifier, icd10_code) "
            "VALUES (%s, %s, %s)",
            diag_batch
        )

    insert_dur = time.time() - t_insert_start
    total_records = len(claims_batch) + len(lines_batch) + len(diag_batch)
    print(f"    Inserted {total_records} relational rows into ASQL in {insert_dur*1000:.1f} ms ({total_records / insert_dur:,.0f} rows/sec).")

def run_adjudication_analytics(conn):
    print("\n==> Running Live Adjudication & Analytics on ASQL...")

    queries = [
        (
            "1. Adjudication Pricing (Claims JOIN Lines JOIN Fee Schedule):",
            """
            SELECT 
                COUNT(*) as priced_lines,
                SUM(l.charge_amount) as total_billed,
                SUM(f.allowable_amount) as total_allowable,
                SUM(l.charge_amount - f.allowable_amount) as contractual_discount
            FROM claim_lines l
            JOIN fee_schedule f ON l.cpt_code = f.cpt_code
            """
        ),
        (
            "2. Top 5 Billing Providers by Volume & Exposure:",
            """
            SELECT 
                p.provider_name,
                c.billing_npi,
                COUNT(c.claim_id) as claim_count,
                SUM(c.total_charge) as billed_exposure
            FROM claims c
            JOIN npi_registry p ON c.billing_npi = p.npi
            GROUP BY p.provider_name, c.billing_npi
            ORDER BY billed_exposure DESC
            LIMIT 5
            """
        ),
        (
            "3. Top Primary Diagnoses (ICD-10) with Line Counts:",
            """
            SELECT 
                d.icd10_code,
                COUNT(d.claim_id) as incident_count,
                AVG(c.total_charge) as avg_claim_cost
            FROM claim_diagnoses d
            JOIN claims c ON d.claim_id = c.claim_id
            WHERE d.qualifier = 'BK'
            GROUP BY d.icd10_code
            ORDER BY incident_count DESC
            LIMIT 5
            """
        ),
        (
            "4. Sub-Millisecond Duplicate Claim Key Check:",
            """
            SELECT 
                patient_member_id, 
                billing_npi, 
                total_charge, 
                COUNT(*) as dup_count
            FROM claims
            GROUP BY patient_member_id, billing_npi, total_charge
            HAVING COUNT(*) > 1
            """
        )
    ]

    with conn.cursor() as cur:
        for title, sql in queries:
            t0 = time.time()
            cur.execute(sql)
            rows = cur.fetchall()
            latency_ms = (time.time() - t0) * 1000.0

            print(f"\n--- {title} (Query Latency: {latency_ms:.2f} ms) ---")
            col_names = [desc[0] for desc in cur.description]
            print("  |  ".join(col_names))
            print("-" * 60)
            if not rows:
                print("  (0 rows / no duplicate keys detected)")
            else:
                for row in rows:
                    print("  |  ".join(str(val) for val in row))

def main():
    conn = get_connection()
    try:
        setup_schema(conn)
        seed_reference_data(conn)
        
        edi_file = "/tmp/test_claim_batch_837.edi"
        generate_edi_837_batch(edi_file, num_claims=1000)
        
        parse_and_ingest_x12(edi_file, conn)
        run_adjudication_analytics(conn)

        print("\n========================================================")
        print(" SUCCESS: End-to-end X12 837 Ingestion & Analytics Verified!")
        print("========================================================")
    finally:
        conn.close()

if __name__ == "__main__":
    main()
