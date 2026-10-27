# Warehouse Touches (Grain Native Reporting)

A high-performance warehouse touches reporting service written natively in Grain for AIOS.
Replaces the legacy PHP/mPDF touches reporting quartet (`quickreportview`, `quickreportpdf`, `quickreportxlsx`, `quickreport`) with zero-dependency native document generation and sub-millisecond execution.

## Endpoints

- `GET /` or `GET /quickreportview`: Interactive operator view (summary metrics cards, touch mix visual breakdown, ranked table, date filtering).
- `GET /quickreportpdf`: High-speed vector PDF generator rendering the 26-column touches grid in landscape letter format with highlighted contributing columns and running pagination headers/footers (`pdf_render`).
- `GET /quickreportxlsx`: Native OpenXML Excel spreadsheet export (`.xlsx`) with frozen panes, cell formats, number styling, and column widths (`xlsx_create`).
- `GET /quickreport`: Direct JSON rollup endpoint without PHP-FPM overhead.
- `GET /health`: Health check endpoint.

## Native Document Engine Primitives

- `pdf_render(html, [css], [options])`: Headless HTML/CSS vector PDF 1.4 compiler with font support (Helvetica, Helvetica-Bold, Helvetica-Oblique, Helvetica-BoldOblique), automatic pagination, multi-page headers/footers, and exact cross-reference tables.
- `xlsx_create(spec)`: Zero-dependency OpenXML spreadsheet generator assembling zipped XML parts (`[Content_Types].xml`, `_rels/.rels`, `xl/workbook.xml`, `xl/styles.xml`, `xl/worksheets/sheet1.xml`).
- `zip_create(entries, [level])`: Native PKZIP archive generator using RFC 1951 DEFLATE compression.
- `unzip(bytes)`: In-tree central directory ZIP decompressor.

## Verification

Run the test suite:

```bash
grain run apps/warehouse_touches/checks.gr
```
