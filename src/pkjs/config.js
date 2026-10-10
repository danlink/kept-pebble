module.exports = [
  { type: 'heading', defaultValue: 'Kept' },
  {
    type: 'text',
    defaultValue: 'In Kept, open Settings → External Access, enable <b>Local MCP access</b> and paste the generated token below. The server must be reachable from this phone.'
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Server' },
      {
        type: 'input',
        messageKey: 'KEPT_URL',
        label: 'Server URL',
        attributes: { placeholder: 'https://kept.example.com', type: 'url', autocapitalize: 'off', autocorrect: 'off' }
      },
      {
        type: 'input',
        messageKey: 'KEPT_TOKEN',
        label: 'API token',
        attributes: { placeholder: 'kept_mcp_…', type: 'password', autocapitalize: 'off', autocorrect: 'off' }
      }
    ]
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Display' },
      { type: 'slider', messageKey: 'MAX_NOTES', label: 'Notes to show', defaultValue: 30, min: 5, max: 30, step: 5 },
      {
        type: 'select',
        messageKey: 'FONT_SIZE',
        label: 'Note font size',
        defaultValue: '1',
        options: [
          { label: 'Small', value: '0' },
          { label: 'Medium', value: '1' },
          { label: 'Large', value: '2' }
        ]
      }
    ]
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'New items' },
      {
        type: 'text',
        defaultValue: 'While a checklist is open on the watch, the phone checks it for items added elsewhere, for example by someone sharing your shopping list.'
      },
      { type: 'toggle', messageKey: 'NEW_ITEM_ALERT', label: 'Vibrate on new items', defaultValue: true },
      {
        type: 'input',
        messageKey: 'NEW_ITEM_PATTERN',
        label: 'Vibration pattern',
        description: '<b>.</b> short, <b>-</b> long, space for a pause. Example: <b>.-</b>',
        defaultValue: '.-',
        attributes: { placeholder: '.-', autocapitalize: 'off', autocorrect: 'off', spellcheck: 'false' }
      },
      {
        type: 'select',
        messageKey: 'POLL_SECONDS',
        label: 'Check every',
        defaultValue: '30',
        options: [
          { label: '10 seconds', value: '10' },
          { label: '30 seconds', value: '30' },
          { label: '1 minute', value: '60' },
          { label: '2 minutes', value: '120' }
        ]
      }
    ]
  },
  { type: 'submit', defaultValue: 'Save' }
];
