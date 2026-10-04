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
  { type: 'submit', defaultValue: 'Save' }
];
