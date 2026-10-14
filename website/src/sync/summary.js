// Adapted from my.hackthenorth.com/src/views/badge/sync/summary.ts.
const PUBLIC_APPS = [
  "2048",
  "67",
  "blocks",
  "blackjack",
  "breakout",
  "bubblewrap",
  "chess",
  "conversation",
  "dance",
  "eightball",
  "etch",
  "flappy",
  "fortune",
  "hangman",
  "hardwarephysics",
  "hopper",
  "invaders",
  "jetpack",
  "kaleido",
  "ktnh",
  "lava",
  "level",
  "lightshow",
  "maze",
  "mines",
  "outrun",
  "pong",
  "reaction",
  "slots",
  "sokoban",
  "teardown",
  "tetris",
  "tictactoe",
  "whack",
];
const GAME_FIELDS = [
  ["67", "67", [["high_score", "High score"]]],
  ["snake", "Snake", [["high_score", "High score"]]],
  [
    "2048",
    "Merge Mode (2048)",
    [
      ["high_score", "High score"],
      ["max_tile", "Largest tile"],
    ],
  ],
  ["breakout", "Breakout", [["high_score", "High score"]]],
  ["tetris", "Tetris", [["high_score", "High score"]]],
  ["flappy", "Flappy", [["high_score", "High score"]]],
  ["dance", "Dance", [["best", "Best score"]]],
  [
    "hopper",
    "Hopper",
    [
      ["best", "Best score"],
      ["levels_completed", "Levels cleared"],
    ],
  ],
  ["invaders", "Invaders", [["best", "Best score"]]],
  ["jetpack", "Jetpack", [["best", "Best score"]]],
  ["outrun", "Outrun", [["best", "Best score"]]],
  ["whack", "Whack", [["best", "Best score"]]],
  ["reaction", "React", [["best_ms", "Best reaction", "ms"]]],
  ["mines", "Mines9", [["best_s", "Fastest clear", "s"]]],
  [
    "blackjack",
    "Blackjack",
    [
      ["wins", "Wins"],
      ["losses", "Losses"],
      ["tie_wins", "Tie wins"],
    ],
  ],
  ["hangman", "Hangman", [["wins", "Wins"]]],
  ["pong", "Pong", [["wins", "Wins"]]],
  ["slots", "Slots", [["richest", "Most coins"]]],
  ["maze", "TiltMaze", [["completed", "Mazes cleared"]]],
  ["bubblewrap", "Bubble Wrap", [["total_popped", "Bubbles popped"]]],
  [
    "chess",
    "Chess",
    [
      ["white_wins", "White wins"],
      ["black_wins", "Black wins"],
      ["draws", "Draws"],
    ],
  ],
  [
    "tictactoe",
    "TicTacToe",
    [
      ["x_wins", "X wins"],
      ["o_wins", "O wins"],
      ["draws", "Draws"],
    ],
  ],
  // Simon is not currently in the launcher, but older badges may have records.
  ["simon", "Simon", [["high_score", "High score"]]],
];
function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}
function number(value) {
  return value !== void 0 &&
    /^\d+$/.test(value) &&
    Number.isSafeInteger(Number(value)) &&
    Number(value) <= 2147483647
    ? Number(value)
    : null;
}
function parseFields(text) {
  const fields = /* @__PURE__ */ new Map();
  for (const line of text.split(/\r?\n/)) {
    if (!line) continue;
    const separator = line.indexOf("=");
    const key = line.slice(0, separator);
    if (separator < 1 || fields.has(key)) return null;
    fields.set(key, line.slice(separator + 1));
  }
  return fields;
}
function decodeBadgeSummary(raw) {
  if (!raw) return null;
  try {
    if (raw.length > 65536) throw new Error();
    const snapshot = JSON.parse(raw);
    if (
      !isObject(snapshot) ||
      snapshot.v !== 1 ||
      snapshot.kind !== "fs_mirror" ||
      !isObject(snapshot.files)
    )
      throw new Error();
    let partial = snapshot.complete === false;
    const files = /* @__PURE__ */ new Map();
    const seen = /* @__PURE__ */ new Set();
    for (const [rawPath, entry] of Object.entries(snapshot.files)) {
      const path = rawPath.replace(/^\/littlefs\//, "");
      if (seen.has(path)) {
        files.delete(path);
        partial = true;
        continue;
      }
      seen.add(path);
      if (
        !isObject(entry) ||
        typeof entry.text !== "string" ||
        entry.trunc ||
        entry.skip
      ) {
        if (!isObject(entry) || entry.skip !== "binary") partial = true;
        continue;
      }
      if (entry.text.length > 4096) {
        partial = true;
        continue;
      }
      files.set(path, entry.text);
    }
    const configs = /* @__PURE__ */ new Map();
    for (const [path, text] of Array.from(files)) {
      if (!path.startsWith("config/") || !path.endsWith(".cfg")) continue;
      const fields = parseFields(text);
      if (fields) configs.set(path, fields);
      else partial = true;
    }
    const config = (slug) => configs.get(`config/${slug}.cfg`);
    const unlockedGames = new Set(
      (config("system")?.get("launcher_order") || "").split(",")
    );
    const games = GAME_FIELDS.filter(
      ([slug]) => slug === "snake" || unlockedGames.has(slug) || config(slug)
    ).map(([slug, name, keys]) => ({
      name,
      metrics: keys.flatMap(([key, label, unit]) => {
        const value = number(config(slug)?.get(key));
        return value === null || (unit && value === 0)
          ? []
          : [{ label, value, unit }];
      }),
    }));
    const sokoban = config("sokoban");
    const levels = Array.from({ length: 12 }, (_, level) =>
      number(sokoban?.get(`best${level}`))
    );
    if (sokoban || unlockedGames.has("sokoban"))
      games.push({
        name: "Sokoban",
        metrics: sokoban
          ? [
              {
                label: "Levels cleared",
                value: levels.filter((moves) => moves !== null && moves > 0)
                  .length,
                unit: "/ 12",
              },
            ]
          : [],
      });
    const launcher = new Set(
      config("system")?.get("launcher_order")?.split(",") || []
    );
    const appCount = PUBLIC_APPS.filter((slug) => launcher.has(slug)).length;
    const regions = config("sponsors")?.get("regions")?.split(",").map(number);
    const validRegions =
      regions?.length === 6 && regions.every((n) => n !== null && n <= 64);
    const regionTotal = validRegions
      ? regions.reduce((sum, n) => sum + (n || 0), 0)
      : 0;
    const sponsorTotal =
      regionTotal > 0 && regionTotal <= 96 ? regionTotal : 64;
    let sponsorCount = 0;
    const contacts = /* @__PURE__ */ new Set();
    for (const [path, fields] of Array.from(configs)) {
      const table = number(fields.get("table"));
      if (
        table &&
        table <= 65535 &&
        path === `config/sponsors/${String(table).padStart(2, "0")}.cfg` &&
        fields.get("version") === "1" &&
        /^[a-z0-9-]{0,31}$/.test(fields.get("slug") ?? "!")
      )
        sponsorCount++;
      const contact = /^config\/contacts\/([a-z0-9_-]{1,63})\.cfg$/i.exec(path);
      if (contact && fields.get("badge_id") === contact[1])
        contacts.add(contact[1]);
    }
    let pals = 0;
    try {
      const wallet = JSON.parse(files.get("solana.json") || "{}");
      const items = new Set(
        isObject(wallet) && Array.isArray(wallet.all_items)
          ? wallet.all_items
          : []
      );
      pals = Array.from({ length: 9 }, (_, i) => String(i + 1)).filter((id) =>
        items.has(id)
      ).length;
      const identity = JSON.parse(files.get("identity.json") || "{}");
      if (
        snapshot.provisioned === true &&
        isObject(identity) &&
        identity.role === "organizer"
      )
        pals = 9;
    } catch {
      partial = true;
    }
    const progress = [
      { label: "Apps discovered", count: appCount, total: PUBLIC_APPS.length },
      {
        label: "Sponsor booths visited",
        count: sponsorCount,
        total: sponsorTotal,
      },
      { label: "Solana scavenger hunt", count: pals, total: 9 },
    ];
    const total = progress.reduce((sum, row) => sum + row.total, 0);
    const done = progress.reduce(
      (sum, row) => sum + Math.min(row.count, row.total),
      0
    );
    return {
      games,
      progress,
      percent: Math.floor((done * 100) / total),
      contacts: contacts.size,
      partial,
    };
  } catch {
    return {
      error:
        "We couldn\u2019t read your saved game data. Sync your badge again to update it.",
    };
  }
}
export { PUBLIC_APPS, decodeBadgeSummary };
