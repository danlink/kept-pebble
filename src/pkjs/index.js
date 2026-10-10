var Clay = require('pebble-clay');
var clayConfig = require('./config');
var kept = require('./kept');

var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

// Must match the enums in src/c/main.c.
var TYPE = {
  LIST_BEGIN: 1, LIST_ITEM: 2, LIST_END: 3,
  NOTE_BEGIN: 4, BODY_CHUNK: 5, ITEM: 6, NOTE_END: 7,
  TOGGLED: 8, CREATED: 9, ERROR: 10, SETTINGS: 11, NEW_ITEMS: 12
};
var CMD = { NONE: 0, LIST: 1, OPEN: 2, TOGGLE: 3, CREATE: 4, INSERT: 5, DELETE: 6 };
var DEFAULT_FONT_SIZE = 1;

var SETTINGS_KEY = 'kept-settings';
var REQUEST_TIMEOUT_MS = 10000;
var MAX_LIST_PAGES = 5;
var DEFAULT_POLL_SECONDS = 30;

// The note open on the watch. items are its checklist rows in the watch's order (the watch
// sends row indices); knownIds are the filled item ids last sent, to spot items added elsewhere.
var openNote = { id: 0, items: [], knownIds: null };

// ---- Settings ---------------------------------------------------------------

function loadSettings() {
  try {
    return JSON.parse(localStorage.getItem(SETTINGS_KEY)) || {};
  } catch (e) {
    return {};
  }
}

function saveSettings(settings) {
  localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
}

function settingValue(entry) {
  return entry && typeof entry === 'object' && 'value' in entry ? entry.value : entry;
}

// ---- AppMessage queue -------------------------------------------------------

var queue = [];
var sending = false;
var attempts = 0;

function send(message) {
  queue.push(message);
  pump();
}

function pump() {
  if (sending || !queue.length) return;
  sending = true;
  Pebble.sendAppMessage(queue[0], function () {
    queue.shift();
    attempts = 0;
    sending = false;
    pump();
  }, function () {
    sending = false;
    if (++attempts > 3) {
      console.log('Dropping message after retries: ' + JSON.stringify(queue[0]));
      queue.shift();
      attempts = 0;
    }
    setTimeout(pump, 250);
  });
}

// Drops not-yet-sent note content when the watch opens a different note.
function dropQueuedNoteMessages() {
  var keepFirst = sending ? queue.slice(0, 1) : [];
  var rest = (sending ? queue.slice(1) : queue).filter(function (message) {
    return message.TYPE < TYPE.NOTE_BEGIN || message.TYPE > TYPE.NOTE_END;
  });
  queue = keepFirst.concat(rest);
}

function sendError(cmd, text, noteId) {
  send({ TYPE: TYPE.ERROR, CMD: cmd, NOTE_ID: noteId || 0, TEXT: kept.utf8Truncate(text, 120) });
}

// ---- Kept API ---------------------------------------------------------------

function keptRequest(method, path, body, onSuccess, onError) {
  var settings = loadSettings();
  if (!settings.url || !settings.token) {
    onError('Open the Kept app settings in the Pebble phone app and enter server URL and token.');
    return;
  }
  var xhr = new XMLHttpRequest();
  var finished = false;
  var timer = setTimeout(function () {
    if (finished) return;
    finished = true;
    xhr.abort();
    onError('Kept server did not respond in time.');
  }, REQUEST_TIMEOUT_MS);

  xhr.onreadystatechange = function () {
    if (xhr.readyState !== 4 || finished) return;
    finished = true;
    clearTimeout(timer);
    var payload = null;
    try { payload = xhr.responseText ? JSON.parse(xhr.responseText) : null; } catch (e) { payload = null; }
    if (xhr.status >= 200 && xhr.status < 300) onSuccess(payload);
    else onError(kept.errorMessage(xhr.status, payload));
  };
  // Unique query parameter so no phone-side HTTP cache can serve a stale note.
  var url = settings.url + path;
  if (method === 'GET') url += (path.indexOf('?') < 0 ? '?' : '&') + '_=' + Date.now();
  xhr.open(method, url, true);
  xhr.setRequestHeader('Authorization', 'Bearer ' + settings.token);
  xhr.setRequestHeader('Accept', 'application/json');
  if (body !== undefined) xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.send(body === undefined ? null : JSON.stringify(body));
}

// ---- Commands ---------------------------------------------------------------

// ---- New-item check -----------------------------------------------------------
// While a checklist is open on the watch, it is re-read every few seconds. Kept's realtime
// socket only accepts browser sessions, not the API token, so this has to poll.

var pollTimer = null;

function stopPolling() {
  if (pollTimer) clearTimeout(pollTimer);
  pollTimer = null;
}

function schedulePoll() {
  stopPolling();
  var settings = loadSettings();
  if (!openNote.id || !openNote.knownIds || settings.newItemAlert === false) return;
  var seconds = Number(settings.pollSeconds) || DEFAULT_POLL_SECONDS;
  pollTimer = setTimeout(pollOpenNote, Math.max(10, seconds) * 1000);
}

function pollOpenNote() {
  pollTimer = null;
  var noteId = openNote.id;
  enqueue(function (finish) {
    keptRequest('GET', '/api/notes/' + noteId, undefined, function (note) {
      finish();
      if (openNote.id !== noteId) return;
      // A change from the watch is queued; it resends the note itself if needed.
      if (!working && note.isCbox && (kept.countNewItems(note, openNote.knownIds) ||
          !kept.sameItems(kept.checklistItems(note), openNote.items))) {
        showNote(noteId, note, -1);
      } else {
        schedulePoll();
      }
    }, function () {
      finish();
      schedulePoll();
    });
  });
}

function closeNote() {
  stopPolling();
  openNote = { id: 0, items: [], knownIds: null };
}

function listNotes() {
  var max = Math.min(Number(loadSettings().maxNotes) || kept.LIMITS.maxNotes, kept.LIMITS.maxNotes);
  var cards = [];
  var pages = 0;

  function fetchPage(cursor) {
    var path = '/api/notes?view=card&limit=80' + (cursor ? '&cursor=' + encodeURIComponent(cursor) : '');
    keptRequest('GET', path, undefined, function (page) {
      pages++;
      cards = cards.concat((page && page.notes) || []);
      var activeCount = cards.filter(kept.isActiveNote).length;
      if (page && page.nextCursor && activeCount < max && pages < MAX_LIST_PAGES) {
        fetchPage(page.nextCursor);
        return;
      }
      var rows = kept.selectNotes(cards, max);
      send({ TYPE: TYPE.LIST_BEGIN, COUNT: rows.length });
      rows.forEach(function (row, index) {
        send({ TYPE: TYPE.LIST_ITEM, INDEX: index, NOTE_ID: row.id, TITLE: row.title, SUBTITLE: row.subtitle, FLAGS: row.flags });
      });
      send({ TYPE: TYPE.LIST_END, COUNT: rows.length });
    }, function (message) {
      sendError(CMD.LIST, message);
    });
  }

  fetchPage(null);
}

// ---- Note commands ----------------------------------------------------------
// Kept requests for the open note run one at a time, so two quick ticks cannot both read
// the note and then overwrite each other's change.

var work = [];
var working = false;

function enqueue(job) {
  work.push(job);
  if (!working) runNext();
}

function runNext() {
  var job = work.shift();
  working = !!job;
  if (!job) return;
  var finished = false;
  job(function () {
    if (finished) return;
    finished = true;
    runNext();
  });
}

// Sends the note to the watch. selectIndex is the checklist row to highlight afterwards, or -1.
function openNoteById(noteId, selectIndex) {
  dropQueuedNoteMessages();
  // A reload of the same note keeps the old rows so ticks sent meanwhile still map.
  if (openNote.id !== noteId) {
    stopPolling();
    openNote = { id: noteId, items: [], knownIds: null };
  }
  enqueue(function (finish) {
    keptRequest('GET', '/api/notes/' + noteId, undefined, function (note) {
      finish();
      if (openNote.id !== noteId) return;
      if (note.locked && note.lockedContentAvailable !== true) {
        sendError(CMD.OPEN, 'This note is locked.', noteId);
        return;
      }
      showNote(noteId, note, selectIndex);
    }, function (message) {
      finish();
      sendError(CMD.OPEN, message, noteId);
    });
  });
}

// Sends a fetched note to the watch. When the same checklist was already shown, items with
// text that were not there before are announced with the configured vibration.
function showNote(noteId, note, selectIndex) {
  dropQueuedNoteMessages();
  var added = openNote.knownIds && note.isCbox ? kept.countNewItems(note, openNote.knownIds) : 0;
  var flags = kept.noteFlags(note);
  var title = kept.utf8Truncate(kept.htmlToText(note.noteTitle).replace(/\s+/g, ' '), kept.LIMITS.noteTitleBytes);
  if (note.isCbox) {
    openNote.items = kept.checklistItems(note);
    send({ TYPE: TYPE.NOTE_BEGIN, NOTE_ID: noteId, TITLE: title, FLAGS: flags, COUNT: openNote.items.length });
    openNote.items.forEach(function (item, index) {
      send({ TYPE: TYPE.ITEM, NOTE_ID: noteId, INDEX: index, TEXT: item.text, DONE: item.done ? 1 : 0, INDENT: item.indent });
    });
  } else {
    send({ TYPE: TYPE.NOTE_BEGIN, NOTE_ID: noteId, TITLE: title, FLAGS: flags, COUNT: 0 });
    kept.bodyChunks(note).forEach(function (chunk) {
      send({ TYPE: TYPE.BODY_CHUNK, NOTE_ID: noteId, TEXT: chunk });
    });
  }
  send({ TYPE: TYPE.NOTE_END, NOTE_ID: noteId, INDEX: selectIndex >= 0 ? selectIndex : -1 });
  var settings = loadSettings();
  if (added) console.log('New items in note ' + noteId + ': ' + added);
  if (added && settings.newItemAlert !== false) {
    send({ TYPE: TYPE.NEW_ITEMS, NOTE_ID: noteId, COUNT: added, TEXT: kept.vibePattern(settings.newItemPattern) });
  }
  openNote.knownIds = note.isCbox ? kept.filledItemIds(note) : null;
  schedulePoll();
}

// Reads the note, changes its checkBoxes with change(note), and saves them. change returns
// the new checkBoxes, or null when the item it needs is gone. On failure the note is sent
// to the watch again so the watch drops its optimistic change.
function updateCheckBoxes(cmd, noteId, change, onSaved) {
  enqueue(function (finish) {
    function fail(message) {
      finish();
      sendError(cmd, message, noteId);
      if (openNote.id === noteId) openNoteById(noteId, -1);
    }
    keptRequest('GET', '/api/notes/' + noteId, undefined, function (note) {
      var checkBoxes = change(note);
      if (!checkBoxes) {
        fail('This item was changed elsewhere.');
        return;
      }
      keptRequest('PATCH', '/api/notes/' + noteId, { checkBoxes: checkBoxes, isCbox: true }, function () {
        finish();
        onSaved(checkBoxes);
      }, fail);
    }, fail);
  });
}

// The watch has already moved the item (and its sub-items) into the other block; this does
// the same to openNote.items, so later row indices match, and then saves it to Kept.
function toggleItem(noteId, index, done) {
  var item = openNote.id === noteId ? openNote.items[index] : null;
  if (!item) {
    sendError(CMD.TOGGLE, 'Reopen the note and try again.', noteId);
    return;
  }
  openNote.items = kept.toggledRows(openNote.items, index, done);
  updateCheckBoxes(CMD.TOGGLE, noteId, function (note) {
    return kept.toggledCheckBoxes(note, item.id, done);
  }, function (checkBoxes) {
    // If the note was also changed elsewhere, the watch's rows are now off: resend them.
    if (working || openNote.id !== noteId) return;
    var saved = kept.checklistItems({ checkBoxes: checkBoxes });
    if (!kept.sameItems(saved, openNote.items)) openNoteById(noteId, -1);
  });
}

// Inserts a dictated checklist item below the item at afterIndex (-1 = top), then resends
// the note with the new item highlighted.
function insertItem(noteId, afterIndex, text) {
  if (openNote.id !== noteId) {
    sendError(CMD.INSERT, 'Reopen the note and try again.', noteId);
    return;
  }
  if (!text || !String(text).replace(/\s+/g, '')) {
    sendError(CMD.INSERT, 'Nothing to add.', noteId);
    return;
  }
  var after = afterIndex >= 0 ? openNote.items[afterIndex] : null;
  if (afterIndex >= 0 && !after) {
    sendError(CMD.INSERT, 'Reopen the note and try again.', noteId);
    return;
  }
  var position = -1;
  updateCheckBoxes(CMD.INSERT, noteId, function (note) {
    var result = kept.insertedCheckBoxes(note, after ? after.id : null, text, Date.now());
    if (!result) return null;
    position = result.index;
    // Dictated on the watch, so not news to announce.
    if (openNote.id === noteId && openNote.knownIds) openNote.knownIds[String(result.id)] = true;
    return result.checkBoxes;
  }, function () {
    openNoteById(noteId, position);
  });
}

// Deletes the checklist item at index, then resends the note so indices match Kept again.
function deleteItem(noteId, index) {
  var item = openNote.id === noteId ? openNote.items[index] : null;
  if (!item) {
    sendError(CMD.DELETE, 'Reopen the note and try again.', noteId);
    return;
  }
  updateCheckBoxes(CMD.DELETE, noteId, function (note) {
    return kept.removedCheckBoxes(note, item.id);
  }, function () {
    openNoteById(noteId, -1);
  });
}

function sendSettings() {
  var size = Number(loadSettings().fontSize);
  send({ TYPE: TYPE.SETTINGS, FONT_SIZE: size >= 0 && size <= 2 ? size : DEFAULT_FONT_SIZE });
}

function createNote(text) {
  if (!text || !String(text).replace(/\s+/g, '')) {
    sendError(CMD.CREATE, 'Nothing to save.');
    return;
  }
  keptRequest('POST', '/api/notes', kept.createNotePayload(text), function (note) {
    send({ TYPE: TYPE.CREATED, NOTE_ID: (note && note.id) || 0 });
    listNotes();
  }, function (message) {
    sendError(CMD.CREATE, message);
  });
}

// ---- Pebble events ----------------------------------------------------------

Pebble.addEventListener('ready', function () {
  console.log('Kept pkjs ready');
  sendSettings();
  listNotes();
});

Pebble.addEventListener('appmessage', function (e) {
  var msg = e.payload || {};
  switch (msg.CMD) {
    case CMD.LIST: closeNote(); listNotes(); break;
    case CMD.OPEN: openNoteById(msg.NOTE_ID, -1); break;
    case CMD.TOGGLE: toggleItem(msg.NOTE_ID, msg.INDEX, !!msg.DONE); break;
    case CMD.CREATE: createNote(msg.TEXT); break;
    case CMD.INSERT: insertItem(msg.NOTE_ID, msg.INDEX, msg.TEXT); break;
    case CMD.DELETE: deleteItem(msg.NOTE_ID, msg.INDEX); break;
    default: console.log('Unknown command: ' + JSON.stringify(msg));
  }
});

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var values = clay.getSettings(e.response, false);
  var previous = loadSettings();
  var token = String(settingValue(values.KEPT_TOKEN) || '').trim();
  saveSettings({
    url: kept.normalizeBaseUrl(settingValue(values.KEPT_URL)),
    token: token || previous.token || '',
    maxNotes: Number(settingValue(values.MAX_NOTES)) || kept.LIMITS.maxNotes,
    fontSize: Number(settingValue(values.FONT_SIZE)),
    newItemAlert: settingValue(values.NEW_ITEM_ALERT) !== false,
    newItemPattern: kept.vibePattern(settingValue(values.NEW_ITEM_PATTERN)),
    pollSeconds: Number(settingValue(values.POLL_SECONDS)) || DEFAULT_POLL_SECONDS
  });
  schedulePoll();
  sendSettings();
  listNotes();
});
