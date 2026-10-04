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
  if (note.isCbox && items.length) {
    var summary = checklistSummary(items);
    if (title) {
      preview = summary.firstOpen ? summary.count + ' · ' + summary.firstOpen : summary.count;
    } else {
      title = summary.firstOpen || 'Checklist';
      preview = summary.count;
    }
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

function checklistItems(note) {
  return (note.checkBoxes || []).slice(0, LIMITS.maxItems).map(function (item) {
    return {
      id: item.id,
      text: utf8Truncate(oneLine(htmlToText(item.data)) || ' ', LIMITS.itemBytes),
      done: !!item.done,
      indent: Math.max(0, Math.min(3, Number(item.indentLevel) || 0))
    };
  });
}

function bodyChunks(note) {
  var text = htmlToText(note.noteBody);
  if (!text) text = '(empty note)';
  return utf8Chunks(utf8Truncate(text, LIMITS.bodyBytes), LIMITS.chunkBytes);
}

// Returns the checkBoxes array with one item flipped, or null if it no longer exists.
function toggledCheckBoxes(note, itemId, done) {
  var found = false;
  var next = (note.checkBoxes || []).map(function (item) {
    if (String(item.id) !== String(itemId)) return item;
    found = true;
    var copy = {};
    for (var key in item) if (Object.prototype.hasOwnProperty.call(item, key)) copy[key] = item[key];
    copy.done = done;
    return copy;
  });
  return found ? next : null;
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
  bodyChunks: bodyChunks,
  toggledCheckBoxes: toggledCheckBoxes,
  createNotePayload: createNotePayload,
  errorMessage: errorMessage
};
