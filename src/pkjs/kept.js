// Pure helpers for talking to the Kept API and shaping data for the watch.
// Kept to ES5 so it runs in every PebbleKit JS runtime; no Pebble globals here
// so the module can be unit-tested with plain node.

var LIMITS = {
  maxNotes: 30,
  titleBytes: 47,
  subtitleBytes: 63,
  noteTitleBytes: 63,
  bodyBytes: 4000,
  chunkBytes: 200,
  maxItems: 60,
  itemBytes: 79
};

var FLAG_PINNED = 1;
var FLAG_CHECKLIST = 2;

var NAMED_ENTITIES = {
  nbsp: ' ', amp: '&', lt: '<', gt: '>', quot: '"', apos: "'",
  hellip: '…', mdash: '—', ndash: '–', bull: '•',
  lsquo: '‘', rsquo: '’', ldquo: '“', rdquo: '”'
};

function decodeEntities(text) {
  return text.replace(/&(#x[0-9a-f]+|#\d+|[a-z]+);/gi, function (match, entity) {
    if (entity.charAt(0) === '#') {
      var code = entity.charAt(1).toLowerCase() === 'x'
        ? parseInt(entity.slice(2), 16)
        : parseInt(entity.slice(1), 10);
      if (!code || code > 0x10ffff) return match;
      if (code > 0xffff) {
        code -= 0x10000;
        return String.fromCharCode(0xd800 + (code >> 10), 0xdc00 + (code & 0x3ff));
      }
      return String.fromCharCode(code);
    }
    var named = NAMED_ENTITIES[entity.toLowerCase()];
    return named === undefined ? match : named;
  });
}

// Kept stores note bodies and checklist items as HTML; the watch wants plain text.
function htmlToText(html) {
  var text = String(html || '')
    .replace(/\r\n?/g, '\n')
    .replace(/<(script|style)[^>]*>[\s\S]*?<\/\1>/gi, '')
    .replace(/<br\s*\/?>/gi, '\n')
    .replace(/<li[^>]*>/gi, '• ')
    .replace(/<\/(p|div|li|h[1-6]|blockquote|pre|tr|ul|ol)>/gi, '\n')
    .replace(/<[^>]*>/g, '');
  text = decodeEntities(text)
    .replace(/[ \t ]+\n/g, '\n')
    .replace(/[ \t ]{2,}/g, ' ')
    .replace(/\n{3,}/g, '\n\n');
  return text.replace(/^\s+|\s+$/g, '');
}

function oneLine(text) {
  return String(text || '').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
}

function utf8Length(codePoint) {
  if (codePoint < 0x80) return 1;
  if (codePoint < 0x800) return 2;
  if (codePoint < 0x10000) return 3;
  return 4;
}

// Splits text into pieces of at most maxBytes UTF-8 bytes without cutting a character.
function utf8Chunks(text, maxBytes) {
  var chunks = [];
  var current = '';
  var currentBytes = 0;
  for (var i = 0; i < text.length; i++) {
    var ch = text.charAt(i);
    var code = text.charCodeAt(i);
    if (code >= 0xd800 && code <= 0xdbff && i + 1 < text.length) {
      ch += text.charAt(++i);
      code = 0x10000;
    }
    var bytes = utf8Length(code);
    if (currentBytes + bytes > maxBytes) {
      chunks.push(current);
      current = '';
      currentBytes = 0;
    }
    current += ch;
    currentBytes += bytes;
  }
  if (current) chunks.push(current);
  return chunks;
}

function utf8Truncate(text, maxBytes) {
  var chunks = utf8Chunks(String(text || ''), maxBytes);
  if (chunks.length <= 1) return chunks[0] || '';
  var head = utf8Chunks(chunks[0], maxBytes - 3)[0];
  return head + '…';
}

function normalizeBaseUrl(url) {
  var trimmed = oneLine(url).replace(/\/+$/, '');
  if (!trimmed) return '';
  if (!/^https?:\/\//i.test(trimmed)) trimmed = 'https://' + trimmed;
  return trimmed;
}

function isActiveNote(note) {
  return !!note && !note.archived && !note.trashed && !note.locked;
}

function noteFlags(note) {
  return (note.pinned ? FLAG_PINNED : 0) | (note.isCbox ? FLAG_CHECKLIST : 0);
}

function checklistSummary(items) {
  var done = 0;
  var firstOpen = '';
  for (var i = 0; i < items.length; i++) {
    if (items[i].done) done++;
    else if (!firstOpen) firstOpen = oneLine(htmlToText(items[i].data));
  }
  return { count: done + '/' + items.length + ' done', firstOpen: firstOpen };
}

// Maps a card from GET /api/notes?view=card to the row shown in the watch list.
function noteRow(note) {
  var items = note.checkBoxes || [];
  var title = oneLine(note.noteTitle);
  var preview;
  if (title) {
    // Titled notes show only their title; content previews are for untitled notes.
    preview = '';
  } else if (note.isCbox && items.length) {
    var summary = checklistSummary(items);
    title = summary.firstOpen || 'Checklist';
    preview = summary.count;
  } else {
    preview = oneLine(note.previewText || htmlToText(note.noteBody));
    if (!title) {
      title = preview || 'Untitled';
      preview = '';
    }
  }
  return {
    id: note.id,
    title: utf8Truncate(title, LIMITS.titleBytes),
    subtitle: utf8Truncate(preview, LIMITS.subtitleBytes),
    flags: noteFlags(note)
  };
}

// Keeps active notes only, pinned first, otherwise in Kept's own order.
function selectNotes(cards, max) {
  var active = [];
  for (var i = 0; i < cards.length; i++) {
    if (isActiveNote(cards[i])) active.push({ note: cards[i], order: i });
  }
  active.sort(function (a, b) {
    var pinDiff = (b.note.pinned ? 1 : 0) - (a.note.pinned ? 1 : 0);
    return pinDiff || a.order - b.order;
  });
  return active.slice(0, max).map(function (entry) { return noteRow(entry.note); });
}

function indentLevel(item) {
  return Math.max(0, Math.min(3, Number(item.indentLevel) || 0));
}

function copyOf(item) {
  var copy = {};
  for (var key in item) if (Object.prototype.hasOwnProperty.call(item, key)) copy[key] = item[key];
  return copy;
}

// The checklist as the watch shows it: open items, then checked items, each in stored
// order. Every write from the watch stores the checklist in this order too. indent is
// Kept's stored level; the watch caps it per block for display, as Kept does.
function displayRows(note) {
  var boxes = note.checkBoxes || [];
  var ordered = boxes.filter(function (item) { return !item.done; })
    .concat(boxes.filter(function (item) { return item.done; }));
  return ordered.map(function (item) {
    return { id: item.id, done: !!item.done, indent: indentLevel(item), box: item };
  });
}

function storedBoxes(rows) {
  return rows.map(function (row) {
    var copy = copyOf(row.box);
    copy.done = row.done;
    return copy;
  });
}

function checklistItems(note) {
  return displayRows(note).slice(0, LIMITS.maxItems).map(function (row) {
    return {
      id: row.id,
      text: utf8Truncate(oneLine(htmlToText(row.box.data)) || ' ', LIMITS.itemBytes),
      done: row.done,
      indent: row.indent
    };
  });
}

// Sets the row at index, and the rows nested below it in the same block, to done and moves
// them to the top of the checked block. That is also the end of the open block, where
// unchecked rows go. Rows need done and indent. Mirrored by checklist_toggle in src/c/main.c.
function toggledRows(rows, index, done) {
  var first = rows[index];
  var end = index + 1;
  while (end < rows.length && rows[end].done === first.done && rows[end].indent > first.indent) end++;
  var group = rows.slice(index, end).map(function (row) {
    var copy = copyOf(row);
    copy.done = done;
    return copy;
  });
  var rest = rows.slice(0, index).concat(rows.slice(end));
  var at = 0;
  while (at < rest.length && !rest[at].done) at++;
  return rest.slice(0, at).concat(group, rest.slice(at));
}

function sameItems(a, b) {
  if (a.length !== b.length) return false;
  for (var i = 0; i < a.length; i++) {
    if (String(a[i].id) !== String(b[i].id) || a[i].done !== b[i].done || a[i].indent !== b[i].indent) return false;
    if (a[i].text !== undefined && b[i].text !== undefined && a[i].text !== b[i].text) return false;
  }
  return true;
}

// Ids of the checklist items that have text, as a set. Empty rows are left out, so a row
// someone has started but not yet filled in counts as new once it gets its text.
function filledItemIds(note) {
  var ids = {};
  (note.checkBoxes || []).forEach(function (item) {
    if (oneLine(htmlToText(item.data))) ids[String(item.id)] = true;
  });
  return ids;
}

// Number of items with text in note whose ids are not in knownIds.
function countNewItems(note, knownIds) {
  var ids = filledItemIds(note);
  var count = 0;
  for (var id in ids) if (!knownIds[id]) count++;
  return count;
}

// Keeps only . - and spaces (long dashes from phone autocorrect count as -), at most 24;
// falls back to .- when nothing is left.
function vibePattern(text) {
  var pattern = String(text || '')
    .replace(/[_\u2013\u2014\u2212]/g, '-').replace(/[\u00b7\u2022]/g, '.')
    .replace(/[^.\- ]/g, '').replace(/\s+/g, ' ').replace(/^ +| +$/g, '');
  return pattern.slice(0, 24) || '.-';
}

function bodyChunks(note) {
  var text = htmlToText(note.noteBody);
  if (!text) text = '(empty note)';
  return utf8Chunks(utf8Truncate(text, LIMITS.bodyBytes), LIMITS.chunkBytes);
}

function rowIndex(rows, itemId) {
  for (var i = 0; i < rows.length; i++) if (String(rows[i].id) === String(itemId)) return i;
  return -1;
}

// Returns the checkBoxes array after toggledRows, or null if the item no longer exists.
function toggledCheckBoxes(note, itemId, done) {
  var rows = displayRows(note);
  var index = rowIndex(rows, itemId);
  return index < 0 ? null : storedBoxes(toggledRows(rows, index, done));
}

// Returns the checkBoxes array without the item with itemId, or null if it no longer exists.
function removedCheckBoxes(note, itemId) {
  var rows = displayRows(note);
  var index = rowIndex(rows, itemId);
  if (index < 0) return null;
  rows.splice(index, 1);
  return storedBoxes(rows);
}

// Inserts a new open item directly after the item with afterId (at the top when afterId is
// null) and copies its indent. After a checked item it goes to the end of the open block
// instead. Returns { checkBoxes, index } with the new item's row, or null if afterId is gone.
function insertedCheckBoxes(note, afterId, text, now) {
  var rows = displayRows(note);
  var position = 0;
  var indent = 0;
  if (afterId !== null && afterId !== undefined) {
    var after = rowIndex(rows, afterId);
    if (after < 0) return null;
    if (rows[after].done) {
      while (position < rows.length && !rows[position].done) position++;
    } else {
      position = after + 1;
      indent = Number(rows[after].box.indentLevel) || 0;
    }
  }
  var id = now;
  var taken = {};
  rows.forEach(function (row) { taken[String(row.id)] = true; });
  while (taken[String(id)]) id++;
  var box = { id: id, data: escapeHtml(oneLine(text)), done: false, indentLevel: indent };
  rows.splice(position, 0, { id: id, done: false, indent: indent, box: box });
  return { checkBoxes: storedBoxes(rows), index: position, id: id };
}

function escapeHtml(text) {
  return String(text || '')
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

// Payload for POST /api/notes from dictated text (mirrors kept/mcp/server.mjs).
function createNotePayload(text) {
  return {
    noteTitle: '',
    noteBody: escapeHtml(oneLine(text)),
    pinned: false,
    bgColor: '',
    bgImage: '',
    isCbox: false,
    labels: [],
    archived: false,
    trashed: false
  };
}

function errorMessage(status, payload) {
  if (status === 0) return 'Cannot reach Kept server. Check URL and connection.';
  if (status === 401) return 'Token rejected. Check token and that Local MCP access is enabled.';
  if (status === 403) return 'Access denied by Kept.';
  if (status === 404) return 'Not found on Kept server.';
  if (status === 423) return 'Note is locked.';
  if (payload && typeof payload.error === 'string') return utf8Truncate(payload.error, 120);
  return 'Kept returned HTTP ' + status + '.';
}

module.exports = {
  LIMITS: LIMITS,
  FLAG_PINNED: FLAG_PINNED,
  FLAG_CHECKLIST: FLAG_CHECKLIST,
  htmlToText: htmlToText,
  utf8Chunks: utf8Chunks,
  utf8Truncate: utf8Truncate,
  normalizeBaseUrl: normalizeBaseUrl,
  isActiveNote: isActiveNote,
  noteFlags: noteFlags,
  noteRow: noteRow,
  selectNotes: selectNotes,
  checklistItems: checklistItems,
  toggledRows: toggledRows,
  sameItems: sameItems,
  filledItemIds: filledItemIds,
  countNewItems: countNewItems,
  vibePattern: vibePattern,
  bodyChunks: bodyChunks,
  toggledCheckBoxes: toggledCheckBoxes,
  insertedCheckBoxes: insertedCheckBoxes,
  removedCheckBoxes: removedCheckBoxes,
  createNotePayload: createNotePayload,
  errorMessage: errorMessage
};
