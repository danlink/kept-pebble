# Kept for Pebble

A Pebble watchapp for a self-hosted [Kept](../kept) notes server. On the watch you can browse notes, read text notes, tick checklist items, and create new notes by dictation.

![list](screenshots/emery_list.png) ![checklist](screenshots/emery_checklist.png) ![note](screenshots/emery_textnote.png)

## How it works

```
Watch (C)  <--AppMessage-->  PebbleKit JS (phone)  <--HTTPS + Bearer token-->  Kept /api
```

The phone does all networking and HTML-to-text conversion. The watch only receives short plain-text strings.

It uses these Kept API routes, all of which a Kept external-access token is allowed to call:

| Action | Request |
| --- | --- |
| List notes | `GET /api/notes?view=card&limit=80` (paged; archived, trashed and locked notes are hidden; pinned notes come first) |
| Open note | `GET /api/notes/:id` |
| Tick item | `GET /api/notes/:id` then `PATCH /api/notes/:id {checkBoxes}` (the note is re-read first so edits made elsewhere are kept; the item and its sub-items move to the top of the checked block) |
| Add checklist item | `GET /api/notes/:id`, insert the item after the highlighted one (or at the end of the open items when the highlighted one is checked), `PATCH /api/notes/:id {checkBoxes}`, then the note is sent to the watch again |
| Delete checklist item | `GET /api/notes/:id`, remove the item, `PATCH /api/notes/:id {checkBoxes}`, then the note is sent to the watch again |
| Dictate note | `POST /api/notes` |

## Setup

1. In Kept, open **Settings → External Access**, enable **Local MCP access** and copy the `kept_mcp_…` token.
2. Install `build/kept-pebble.pbw` on your watch: `pebble install --phone <ip>`, or open the file with the Pebble app.
3. In the Pebble phone app, open the Kept app's settings and enter your server URL (for example `https://kept.example.com`) and the token. You can also set how many notes to show and the note font size (Small, Medium or Large).

The Kept server must be reachable from the phone, either over public HTTPS or over Tailscale/VPN. The token stays on the phone and is never sent to the watch.

## Controls

- **List:** titled notes show only their title; untitled notes show a preview. Select opens a note. The list reloads from Kept when you come back from a note; hold Select to refresh it manually. "+ New note" starts dictation; it is only shown on watches with a microphone.
- **Checklist:** open items come first; checked items follow in an inverted block, the most recently checked on top. Select ticks or unticks an item together with its indented sub-items: a ticked item moves to the top of the checked block, an unticked one to the end of the open items, and the highlight stays in place on the next item. The change shows at once; if saving fails, the note is reloaded from Kept. Hold Select to open a menu: **Dictate new below** adds a dictated item directly below the highlighted one, with the same indent, and highlights it once saved; **Delete line** removes the highlighted item from Kept.
- **Text note:** Up/Down scroll.

## Development

```bash
pebble build
pebble install --emulator emery
pebble emu-app-config --emulator emery   # enter URL + token
```

The emulator's phone side runs on your computer, so `http://localhost:<port>` works there.

- **Targets:** emery (main), basalt, diorite, chalk.
- **Checklist order:** every write from the watch stores the checklist in Kept as open items, then checked items, which is also how Kept shows it with "move completed items to bottom" turned on. Kept has no checked-at time, so the order of the checked block is what keeps "most recently checked on top". Items checked in Kept itself keep their place in that block. The watch (`checklist_toggle` in `main.c`) and the phone (`toggledRows` in `kept.js`) apply the same move, so row indices stay in sync without resending the note.
- **Protocol:** the message types and command IDs in `src/c/main.c` and `src/pkjs/index.js` must stay in sync.
- **Phone-side logic:** pure helpers (HTML to text, UTF-8-safe chunking, list selection, request payloads) live in `src/pkjs/kept.js` and can be tested with plain node.
