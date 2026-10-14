# Sponsor Table Scanning Prototype

> Historical design record: the shipped map now uses six configurable regions
> and a 65-sponsor roster. See [SPONSOR-MAP-CONFIG.md](SPONSOR-MAP-CONFIG.md)
> for the current event-day layout and console command.

## Purpose

Build a first end-to-end sponsor-table experience for the badge before the
final two-floor floorplan and sponsor roster are available. The prototype
uses a 5×5 grid with 25 numbered tables, a compiled sponsor directory, NFC
scanning from both the central Scanner app and the sponsor app, and persistent
LittleFS state that can be included unchanged in a badge backup.

The prototype is deliberately **table-centric**. Quest completion is the
number of distinct table IDs visited, not the number of recognized sponsors.
The 25 compiled slots are the expected map and completion target, but scans are
forward-compatible with an updated roster or overflow table IDs.

## Research summary

The canonical event experience already calls for a Default-tier sponsor map,
one central Scanner app, booth tags containing a sponsor slug plus table
number, automatic redirection to the map, lit visited tables, sponsor detail,
and completion in the top bar. It also notes that the final layout spans two
floors and that sponsor-tap state belongs in the upload/restore payload
(The Hacker Badge Event Experience, internal planning doc).

The sponsor-facing material promises every sponsor an NFC touch point that
lets hackers track visited booths and learn about the company on-badge
(Sponsor Hacker Badge Tiers Explanation and Sponsor Hacker Badge Technical
Specifications, internal docs).

Nick's hardware notes reinforce that NFC must remain enable-on-demand: a
battery test with NFC, LEDs, and radio active lasted about 5.5 hours,
and the current HAL already powers the antenna down between apps. The official
2026 site supplies useful prototype company names, but the final roster and
descriptions still need an event-owned source of truth
([hackthenorth.com](https://hackthenorth.com/)).

## Prototype scope

Included:

- 25 table slots arranged as a 5×5 grid.
- A compiled sponsor directory with slug, display name, description, and link,
  kept independent of the 25 physical table slots.
- NFC active only while Scanner or Sponsors is foregrounded.
- Shared payload parsing and persistence used by both apps.
- Visited highlighting, selected-cell focus, `visited / total`, and percent.
- A list view containing every known sponsor and its currently learned table.
- Sponsor detail containing name, table, description, link, and visit state.
- Persistent one-file-per-table state under LittleFS.
- Raw slugs and overflow table IDs remain valid when event-day data differs
  from the compiled roster and 5×5 map.
- A new-table modal and two green LED pulses acknowledge successful discovery.
- A test-only hold gesture that reveals table IDs on the map.

Not included:

- Logos or generated image assets.
- The final two-floor physical layout.
- Tag writing or locking; firmware remains read-only.
- Upload-station transport or backend changes. The filesystem contract is
  designed to be backed up wholesale by that future work.
- The post-completion bonus game.

## Tag contract

Tags use one NDEF Well-Known Text record with this ASCII payload:

```text
spon:<sponsor-slug>:<table-id>
```

Example:

```text
spon:solana:1
```

Rules:

- `table-id` is decimal `1..65535`; IDs `1..25` have cells in this prototype's
  map and higher IDs are counted without being placed on that map.
- `sponsor-slug` uses lowercase ASCII letters, digits, and hyphens.
- The complete record must fit in the first 64 bytes read by `hal_nfc`.
- A slug does not have to exist in the compiled directory. Known slugs receive
  their nice name, description, and link; unknown ones are persisted and
  displayed using the raw slug as a best-effort name.
- Sponsor and table are intentionally not a precompiled pair. Any sponsor can
  be assigned to any table on event day.
- Extra delimiters, invalid slug characters, zero, and non-numeric table IDs
  are rejected.
- Re-scanning a valid table is idempotent and reports "already visited."

Static tag lock bits remain the mechanism that prevents arbitrary tags from
being minted at the event; firmware deliberately favors event-day data
flexibility over enforcing a stale compiled roster.

## Compiled directory

`sponsor_catalog.{h,cpp}` owns the expected roster. Each entry contains:

```text
slug, name, description, link
```

The 5×5 table layout is a separate set of expected numbered slots. A table
starts with an unknown `???` identity and learns its sponsor from a valid scan.
This lets, for example, Solana move from table 1 to table 5 without a firmware
rebuild. A slug absent from the directory still identifies the visit; it uses
the slug in the UI and simply has no compiled description or link. The first
directory uses names visible on the official 2026 site; all copy should still
be replaced from the final sponsor source of truth.

## Persistent visit store

One validated file is written atomically per table:

```text
/littlefs/config/sponsors/01.cfg
```

Contents:

```ini
version=1
table=1
slug=solana
visited_unix=0
```

The store writes a `.tmp` file and renames it, mirroring contact persistence.
File names come only from the validated numeric table ID. Counting enumerates
canonical files and validates their version, embedded table ID, and slug rather
than trusting arbitrary `.cfg` files. Every distinct valid table record counts,
including records outside the expected map: scanning a new table 99 after all
25 mapped tables therefore displays `26/25`, while no table-99 grid cell is
invented.

The stored slug is the badge's latest learned assignment for that table. If a
sponsor is scanned at a new table, the new table takes that sponsor identity;
the old table remains visited for completion but returns to `slug=` and `???`.

Because sponsor state lives under `/littlefs/config/sponsors/`, a future
filesystem backup/restore automatically carries it. Sync should treat these
files as the source of truth; no separate progress counter is required.

## UX and controls

### Map view

- Header: `Sponsors  7/25  28%`.
- 5×5 table cells, with unvisited, visited, and focused visual states.
- Unknown table assignments display `???`; learned assignments display compact
  sponsor initials.
- Arrow keys move focus without wrapping.
- `A` opens the focused table's sponsor detail.
- Hold `Start` to reveal numeric table IDs for layout testing.
- `B` opens the list view from the map, or steps back to the map from elsewhere.
- `HOME` returns to the launcher.
- A valid in-app NFC tap records the table immediately, selects it, refreshes
  progress, and shows a short already-visited notice for repeat scans.
- A first visit shows a modal saying `You visited a new sponsor!`, followed by
  the compiled sponsor name or its raw slug when no directory entry exists,
  and pulses all badge LEDs green twice. The table ID remains secondary. It
  closes automatically or with `A`, `B`, or `Start`.
- LittleFS is read once when entering the app or after a valid scan. Arrow-key
  navigation updates only the old and new focused cells in place.

### List view

- Shows every known directory sponsor as a Pokédex-style roster.
- Appends visited slugs that are not in the compiled directory, so an
  event-day sponsor is not lost after its discovery modal closes.
- Each row includes discovery state, a learned table number or `??`, and name.
  Known and unknown sponsors can show overflow locations such as table 99 even
  though those locations have no map cell.
- Up/Down scroll and `A` opens detail. `B` and `Start` both return to the map.

### Detail view

- Sponsor name (or `???`), learned table, visited state, description, and link.
- `B` and `Start` both return to the map.

## Scanner integration

The central Scanner app checks `spon:` payloads before treating text as an app
unlock slug. It uses the same parser/store as the sponsor app. A valid scan:

1. validates the payload shape, numeric table, and safe slug characters;
2. atomically records the table if new;
3. prepares the sponsor app to focus a mapped table, or acknowledge an
   overflow table without inventing a map location;
4. launches the sponsor app.

Non-sponsor text continues through the existing app-unlock flow unchanged.
Malformed `spon:` text is an explicit sponsor-tag error rather than an
"unknown app" card.

If the sponsor app reads a non-sponsor tag, it transfers the already-read text
to Scanner and launches Scanner immediately. The hacker does not need to tap
the same tag a second time.

## Completion and event-day fallback

Progress is:

```text
distinct valid visited table files / expected map table count
```

The app reaches 100% at 25 distinct table IDs and can show an over-complete
numerator such as `26/25` when an overflow or replacement table is scanned. It
does not require every sponsor slug ever associated with those tables. If a
sponsor does not attend, organizers can place correctly encoded table stickers
at the badge/help desk so hackers can finish the table quest; those tags may
use the final event-day slug even if it was not compiled into the firmware.

## Delivery plan

1. **Scan and storage branch:** add the catalog, strict payload parser,
   atomic table store, scanner handling, and progress-app integration.
2. **Map UI branch:** add the Default-tier sponsor app, map/list/detail views,
   in-app NFC handling, scanner redirection, and launcher registration.
3. **Event content follow-up:** replace the prototype directory, set the real
   table count and two-floor layout, and program/lock physical tags.
4. **Hardware verification:** test valid, duplicate, mismatched, malformed,
   and removed-mid-read tags; confirm NFC power-down on app exit; power-cycle
   to prove persistence; and inspect all 25 cells on the 320×240 display.

## Open decisions

- Final sponsor roster, company copy, links, table count, and two-floor map.
- Physical tag type and locking workflow.
- Whether a shared table can host multiple sponsors simultaneously; the first
  UI stores the latest single sponsor assignment for each table.
- Final completion reward and where catch-up stickers will be placed.
