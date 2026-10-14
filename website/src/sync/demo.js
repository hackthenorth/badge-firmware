// Synthetic data only. Never collected from an attendee or uploaded anywhere.
const file = (text) => ({ size: new TextEncoder().encode(text).length, text });
export const demoSnapshot = {
  v: 1,
  kind: "fs_mirror",
  badge_id: "demo-badge",
  provisioned: true,
  complete: true,
  files: {
    "identity.json": file(
      JSON.stringify({
        badge_id: "demo-badge",
        display_name: "Alex Hacker",
        role: "hacker",
      })
    ),
    "config/system.cfg": file(
      "default_apps_version=14\nlauncher_order=badgeid,connect,scanner,sync,share,blindbox,sponsors,workshops,mood,socials,schedule,rules,museum,progress,snake,dice,2048,chess,tetris,flappy,conversation,bubblewrap,ktnh\n"
    ),
    "config/contacts/demo-ada.cfg": file(
      "badge_id=demo-ada\nname=Ada Example\nrole=0\nemail=ada@example.com\nlinkedin=ada-example\ndiscord=ada.example\nreceived_unix=1790000000\n"
    ),
    "config/contacts/demo-sam.cfg": file(
      "badge_id=demo-sam\nname=Sam Example\nrole=4\nemail=sam@example.com\ninstagram=sam.example\nreceived_unix=1790003000\n"
    ),
    "config/snake.cfg": file("high_score=1280\n"),
    "config/2048.cfg": file("high_score=8400\nmax_tile=1024\n"),
    "config/tetris.cfg": file("high_score=3200\n"),
    "config/chess.cfg": file("white_wins=3\nblack_wins=2\ndraws=1\n"),
    "config/sponsors/01.cfg": file(
      "version=1\ntable=1\nslug=demo-partner\nname=Demo partner\n"
    ),
    "config/workshops/demo.cfg": file("name=Build something new\nvisited=1\n"),
    "solana.json": file(
      JSON.stringify({ all_items: ["1", "2", "4", "5", "8"] })
    ),
    "apps/hello/main.lua": file(
      'function on_enter()\n  badge.ui.label("Hello, world!")\nend\n'
    ),
    "apps/hello/icon.bin": { size: 4608, skip: "binary" },
  },
  counts: {
    seen: 13,
    text: 12,
    binary: 1,
    too_big: 0,
    dropped: 0,
    unreadable: 0,
    truncated: 0,
    path_skipped: 0,
  },
};
