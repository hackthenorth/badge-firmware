# Sponsor map event-day configuration

The Sponsors app opens directly to a compact map of six booth regions. It
keeps the existing NFC visit storage and sponsor directory independent of the
physical layout, so sponsor roster or table changes do not require rebuilding
the app.

## Defaults

No setup command is required. With no saved override, the six regions contain:

| Region | A | B | C | D | E | F | Total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Booths | 8 | 4 | 4 | 14 | 28 | 6 | 64 |

These counts reproduce the current 2x4, 4x1, 2x2, 3x5-with-one-open-slot,
7x4, and 1x6 batches. Region D keeps three columns, with its final two booths
centered on the bottom row. Region B is the four-space horizontal row on the
first floor. The sponsor directory contains 64 sponsors, including MLH (Major
League Hacking).
Each region keeps its physical footprint on the 320x240 map. The app chooses a
near-square row/column arrangement and shrinks cells as needed when a count
changes. The map labels the outlined B area as Floor 1 and the remaining areas
as Floor 2. Region letters and counts stay in the console configuration; they
are not shown on the map.

Press `A` on the map for a guide to sponsor taps, with the same flower sticker
shown in Scanner. It explains how table taps record visits, reveal sponsor
information, and contribute to the sponsor quest. Press `A` again for the
selected table's details, or `B` to return to the map.

Hold `Start` on the map to reveal table IDs. `B` switches between the map and
list; `A` on the list opens a sponsor's details. `HOME` exits immediately from
every view.

Table IDs are assigned sequentially across A through F. With the defaults,
region A owns tables 1-8, B owns 9-12, C owns 13-16, D owns 17-30, E owns
31-58, and F owns 59-64. NFC payloads remain:

```text
spon:<sponsor-slug>:<table-id>
```

## Console command

Run `sponsormap` with no arguments to print the one-line command that recreates
the active layout:

```text
badge> sponsormap
sponsormap 8 4 4 14 28 6
```

Set all six region counts in one operation. For example, adding two booths to
region E is:

```text
badge> sponsormap 8 4 4 14 30 6
OK sponsor map saved; reopen Sponsors to refresh
sponsormap 8 4 4 14 30 6
```

The override persists in `/littlefs/config/sponsors.cfg`. Reopen the Sponsors
app after changing it. Restore the compiled defaults with:

```text
sponsormap reset
```

Each region accepts 0-64 booths, the combined visible map accepts 1-96, and
unknown or higher NFC table IDs still persist and count as overflow visits.
When region counts change, regenerate table-tag payloads for any later region
whose sequential starting ID moved.
